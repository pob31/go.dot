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
| `/godot/mount/<id>/name` | `s` | rw | what the show calls this device (2026-09-22) |
| `/godot/mount/<id>/rx` | `T`/`F` | rw | whether what it sends is accepted (2026-09-22) |
| `/godot/mount/<id>/tx` | `T`/`F` | rw | whether cues aimed at it reach the wire (2026-09-22) |
| `/godot/mount/<id>/problem` | `s` | ro | why it cannot be used as declared (2026-09-22) |

**Every row above but the last four readouts became `rw` on 2026-09-22**, when the show settings
window grew a Network tab. They had been read-only since Phase 1, for a reason that was true
then — a mount was a thing you wrote into `show.xml` and reopened the show to change — and stopped
being true the moment a person could type a port into a box during a tech rehearsal. `loaded`,
`nodeCount`, `sent` and `problem` stay read-only: they are what the machine found, not what
anybody decided (PRD §4.10). What carries an edit to the socket is `refreshMountDeclarations`,
called from the after-tick when the show revision moves — see §15.

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
| `document.copy` | `/godot/cmd/document/copy` | `s` ids, space-separated | *(2026-09-18)* a read: copies of the cues as one canonical `<Fragment>`, published at `document/clipboard` |
| `document.paste` | `/godot/cmd/document/paste` | `s` parent, `i` member index, `s` fragment, `[s ids]` | *(2026-09-18)* the fragment's cues enter under NEW ids, intra-fragment references re-pointed, one undo step; the record carries the ids drawn so a replay draws none |
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
  *Reaffirmed 2026-09-17, when a Windows runner's virus scanner made a save miss the driver's
  window and the question was put to the author: the ENGINE keeps saving under the lock - lock
  first so nothing moves, then save, so the file on disk is the final show - and what changes is
  the CLIENTS. Show mode does not OFFER a save: the page's Save button and Ctrl-S go quiet while
  `locked` reads true, and the desktop's show mode (M9) does the same, because "usually we try not
  to save a show mid performance". A script still can. And a save may lag - "Save can have some
  lag so it doesn't interfere with the live show" - so the writer thread's one blind retry became
  three pauses, a third of a second in all; §14.10 carries the correction.*

- **X — The pointer may stand inside every group** (settled 2026-09-16, with the page open): asked
  directly whether a cue inside an automatic or a timeline group is a place the standby may be put,
  the author answered **every group**, and that GO there fires **that one cue**. So residence widens
  to *any cue of this list that is not in a header, a footer or a persistent section*, `standby.set`
  stops refusing a member of a non-manual group, and ▲/▼ from inside one walk that group's members
  and climb out at its ends. What does **not** widen is the step from outside: ▲/▼ still descend
  only into a manual sequence group, or a reader walking a list top to bottom would land on an
  automatic group's first member and the GO meant for the scene would fire one cue of it (decision
  M unchanged). Starting the group from the member the pointer is on is a **second named gesture**,
  decided to exist and deliberately not built in this round; §12.6 proposes `go.from` and says what
  is open about it. The old refusal's recorded reason — a pointer the machine also moves — was
  about the operator's model rather than a race: only GO ever writes the standby.

- **Y — Media arrives through the desktop client, not through the page** (settled 2026-09-16):
  asked whether dropping a sound file into the cue list should make media cues, and dropping one
  onto a media cue should link it, the author answered that the gesture **waits for the C++ client**
  (§14.16) and that nothing is built on the page in the meantime. What forced the question is a
  fact about browsers rather than a preference: **a page is never told a dropped file's path.** It
  is given the name and the bytes, deliberately, so the web client cannot say *"the file is at
  D:\audio\rain.wav"* — it can only offer to COPY one into the show. And `media/@file` is
  deliberately relative to the bundle's `media/` folder, for the reason the parameter table gives
  in as many words: *"a show travels between machines and an absolute path is a fact about the
  machine it was authored on."* So the page's only honest route is an import, and the engine has
  none: `OscQueryServer` answers `GET` and refuses every other method (`:211-216`), there is no
  upload route and no import command. The desktop client is where a real path exists.
  **What this leaves open, for whenever that client starts:** whether an import COPIES into the
  bundle (it must, given the relative path) or is offered a way to reference outside it; whether
  the bytes arriving are a document decision — logged and undoable — or a fact about the disk that
  only the cue naming the file records (§4.10 suggests the second); what a name collision does; and
  whether a locked show refuses one. This is the first gesture where the two clients genuinely
  cannot be the same, which bears on §9's question E and on decision U.

- **Z — One voice per armed member** (settled 2026-09-23, with the Phase 6 plan): GO on a sampler
  group arms every member on a track of its own, and a member that finds no free track does not fail
  — it shows *pending* in words and lands when one frees, §3.9e's waiting claim applied to a voice
  for the first time. Polyphony is bounded by `Show/Audio/@tracks` and by nothing else. The other
  shape, a group declaring a voice count and sharing it among its members, had been the lean since
  2026-09-07 and is not built: a strip's fader rides one run's level and a run's level is its
  track's, so two members on one track would share a fader; and a voice handed out at the press is a
  file made ready at the press, which is exactly the delay fader-start exists to avoid. Answers PRD
  §3.25's *(proposed)* claim shape, §3.27's **Voices.** and §13.15's open question. §16.1, and §16.5
  for the mechanism.

- **AA — Velocity sets the level a clip starts at; pressure rides it while the pad is held**
  (settled 2026-09-23): per clip, both off by default. A velocity of one starts at `velocityFloor`,
  127 at 0 dB, a straight line in dB between; pressure moves the same trim a fader moves, on the
  same scale, and only while the pad is down. The author's words: *"a pad is a fader without a
  motor."* This AMENDS PRD §3.16's gate row and §3.27's *"No pressure, no XY"*, which were written
  against WFS-DIY's MPE-shaped Sampler; XY and per-note pitch stay out, and nothing new reaches the
  audio — pressure writes the node a fader writes. §16.1.

- **AB — A Surfaces tab in Show settings, and a virtual surface panel in the desktop client**
  (settled 2026-09-23): the tab declares each surface — its profile, a port per bank of eight, the
  preset the hardware expects — each strip's role, and the DCAs; the panel draws every strip of
  every surface with a fader, a pad, a name, a state word and a colour, is played with the mouse
  through the commands a surface sends, and is PRD §3.17's redundancy path when the hardware is
  absent. It answers the author's *"look into the show settings to add a control window for this"*.
  §16.1 and §16.7.

- **AC — The engine and the virtual panel first, the generic Mackie bridge second, the D700 layer
  third** (settled 2026-09-23): sampler groups and DCAs are judged on a screen with no hardware and
  driven in CI the same way; real faders come through a Mackie Control bridge any MCU unit answers;
  colour, the three-row display, the rings and the second bank are a layer over it. HUI is not in
  the phase, and the Icon V1 and P1 are new profile words when they arrive. §16.1.

- **AD — The EQ is Go.dot's own** (settled 2026-09-23, with the Phase 9a plan; the recommendation
  taken): a fixed stage on every voice track in `CueOutputPlugin`'s shape — a custom Tracktion
  plugin type, its settings atomics the tick thread writes as it writes the level, no
  `AutomatableParameter`, no message-thread hop, no lock; high-pass, low-pass and four parametric
  bands; the settings nineteen rows on the media cue, every gain resting at 0 dB. Declined:
  Tracktion's own four-band equaliser (a lock on the audio thread, message-thread parameters, no
  filters) and an EQ plugin chosen per cue (parameters a page could not be laid out for). §17.1
  and §17.5.

- **AE — Inserts are a chain on every voice track** (settled 2026-09-23, AGAINST the
  recommendation of PRD §3.18's rack channels): the show declares a plugin set as it declares its
  tracks; every voice carries the whole set, instantiated at open and switched off; a media cue
  says which of the set it switches in and carries its own values; at arm the voice gets them and
  while the cue sounds a `node.set` reaches the voice live. PRD §3.18's *(proposed)* bypassed
  stack, answered yes and placed on the voices rather than in a rack. What it buys is no allocation
  to fail — every voice has every plugin; what it costs is N × P instances (M34) and a set fixed
  at open. Phase 4's `Media/Insert` and `Rack/Channel` stay as they are, for Phase 9b's live rack.
  §17.1 and §17.2.

- **AF — The out-of-process proxy is built first** (settled 2026-09-23, AGAINST the
  recommendation of in-process hosting now): §3.18's sandbox pulled forward from Phase 9 — a custom
  Tracktion plugin type on every voice handing each block to a child process through shared
  memory on a bounded spin with a hard deadline, a miss passing the dry block through, a strip that
  keeps missing marked failed and no longer called, the child hosting the real plugin one instance
  per voice, and scanning out of process always. The author's reason is §3.18's: when a plugin
  dies, the show survives. The cost is that every VST insert waits on the proxy, which is weeks;
  the EQ waits on none of it. A consequence that is a gain: a plugin behind the proxy has no
  Tracktion parameter at all, so §3.4's message-thread handover applies only to inline hosting,
  which is not built. §17.1 and §17.6.

- **AG — The scope: EQ, inserts, the pages draft's §8 contract, the inspectors, tests and
  measurements** (settled 2026-09-23; the recommendation taken): not the surface pages, not the
  virtual panel's rotaries — the pages session's, built on this in the order
  `docs/godot-surface-pages-draft-0.1.md` §11 set. §17.1 and §17.10.

### Open, with the subphase that forces each

| # | Question | Forced by | Fallback if undecided |
|---|---|---|---|
| ~~E~~ | *(settled 2026-09-17, against this table's own fallback — see below)* **Does the Phase 5 desktop UI run in-process or as a separate client?** | Phase 5, but it shapes Phase 2's plugin-parameter handover | ~~assume separate, because that is the stricter assumption and the one PRD §3.2 reads most naturally~~ — the author chose **in-process**, and the law is kept by a rule about ACCESS rather than about processes |
| J | **Should PRD §4.2 record what Tracktion does inside the callback?** Its device callback takes one uncontended `std::shared_lock` per block and its node-player pool uses semaphores; the lipogram can be *enforced* on Go.dot's code and only *measured* on Tracktion's (§11.5). A PRD amendment is the author's to make. | the lipogram test (PR 2.2) | enforce on Go.dot's scopes, report Tracktion's count separately, never hide it |
| ~~K~~ | *(settled 2026-09-06, in PR 2.6, the way this table recommended — see below)* **How does a mount declare what it can do?** `transport` says how to *send* and nothing says whether the target can be *asked*, so `wait: verified` against a write-only device is a cue that cannot succeed and nothing notices until the show. Chataigne carries two booleans per module, `hasInput` and `hasOutput`, for exactly this. Also: whether the answer names the *mechanism* (`oscquery` \| `poll` \| `subscribe` \| `none`) or only the capability. | `verified` (PR 2.6) | a mount-level `readback` enum defaulting to `none`, and a `verified` cue against `none` refused at load — the strictest reading, and the one that cannot fail silently |

**E — SETTLED 2026-09-17: IN PROCESS, AND THE DESKTOP CLIENT GETS AS MUCH SLACK AS IT NEEDS.**
The author was asked before a line of the client was compiled - which was the whole point of
asking, since this subsection had recorded that a compiled client started without an answer would
answer E *by accident*, by whatever was convenient in its first week.

**The answer is in process**, for two reasons they gave. Media, which decision Y established a
browser cannot do at all - a page is never told the path of a file dropped on it - and which a
local process handles without an import route having to exist first. And less code: a second
networked model is a second model to keep in step with the engine's. The page and a remote app
*complement* it, for when somebody is not at the machine.

**THE RANKING IS THE POINT, AND IT IS THE AUTHOR'S OWN PRACTICE RATHER THAN A CONCESSION.**
WFS-DIY already runs this shape: its Android remote drives sixty-four inputs and ten arrays over
bi-directional OSC at fifty hertz, and the desktop plugin has far more than the tablet does. The
tablet is a subset of CONTROLS and not a subset of POWERS. Go.dot is the same: the desktop client
is the richest surface, the page is a useful subset, and neither is a compromise for the other.
Nothing here holds the compiled client down to what a browser can draw, and the page lacking
something is never a reason for the engine to lack it.

**WHAT BELONGS TO A MACHINE STAYS WITH THAT MACHINE.** WFS-DIY's remote has *Find Device* - flash
the screen and sound an alarm so somebody can locate the tablet in a dark venue - and a
finger-pressure calibration, and neither exists anywhere else. Nobody has ever thought that broke
anything, because neither touches the audio. A native file dialog is Go.dot's *Find Device*: the
desktop doing something with its own hardware, which ends in an argument to an ordinary command
that the tablet could have sent had it had the path.

**AND COPYING MEDIA INTO THE BUNDLE IS NOT A CHANGE TO THE SHOW**, which is the reading decision Y
left open and §4.10 settles. The *decision* is the cue naming the file; the bytes landing in
`media/` are a fact about the disk, exactly like the timbre cache the analyser thread already
writes with no command, no record and nobody's undo history disturbed. So the desktop client may
open a dialog, copy the file in itself, and then send one ordinary `node.set` so the cue names it -
logged, undoable, and reachable by any client that ever has a path.

**THE ONE LINE, AND IT PAYS THE AUTHOR BACK RATHER THAN CONSTRAINING THEM: the show changes only
through named commands.** Not for parity's sake. Because that is what puts a change in the undo
history, in the event log, and in a replay of the night it went wrong. A client that edited the
document directly would be taking cues out of an operator's ctrl-Z without telling them, and
`wfg replay` of that session would not reproduce it.

PRD §3.2's law is not bent by any of this, and reading it whole is what shows why. Its slogan -
*"nothing the UI can do that the API cannot"* - is followed immediately by the sentence that
carries the content: *"Every gesture-reachable **action** also exists as a named command… Modifiers
and gestures are an accelerator layer over a complete command set, never the only route."* The
binding word is ACTION. A file dialog is not an action on the show; it is how an operator produces
an argument, exactly as the page's sliders, number boxes and drag-to-reorder are, none of which
exist in the API either and none of which anybody thought broke the law.

**HOW IT REACHES THE ENGINE, WHICH IS A SMALLER QUESTION THAN IT LOOKED.** Both doors already exist
and are already the ones OSC arrives through. `Engine::submit (origin, command, args)` puts an
event on a queue that never blocks its caller and is applied on the tick thread in arrival order,
logged and replayable. `ParameterTree::snapshot()` hands back a `shared_ptr<const TreeSnapshot>`,
so a read is a pointer copy that can neither tear nor show a half-applied tick - the same object
`GET /godot` is answered from. Those two cover reading the show and changing it; the file work
above touches neither, since the bundle's path is published like anything else.

So the client has no need of a `ShowDocument&` that anybody can presently name - and that is
recorded as an observation rather than as a prohibition. **If a day comes when it does need one,
that is worth stopping over**, because it means the API is missing something the tablet is missing
too, and the honest repair is to add the command rather than to reach past it.

**IT IS ALSO THE THREADING THE AUTHOR ASKED FOR** (*"a threaded client would be preferable"*), and
not by coincidence. JUCE requires its components on the message thread and the engine requires its
document on the tick thread, so those two can never touch each other's data directly; the queue and
the snapshot are exactly the hand-off between them, and they are the same hand-off `HostPlayer` and
the OSCQuery server already sit behind. The client therefore adds no new threading rule to this
engine at all, and the interface can never stall GO.

**WHAT THE EVIDENCE SAID, AND WHY IT DID NOT DECIDE IT.** The web client of decision V is a
separate client by construction, holds nothing the engine owns (§14.1) and has driven every gesture
Phase 4 built over a socket without one hole being opened for it. That is a strong argument that
the API is complete enough to build a client against, and it stays true. What it was never an
argument for is a second PROCESS, which buys serialisation and a round trip and nothing the reading
above does not already buy.

**WHAT TO WATCH, since a decision recorded without its failure mode is half recorded.** The one
line is a discipline and not a compiler error: the UI is linked against `wfg_engine`, so nothing
stops somebody reaching for the document. What makes that visible rather than trusted is the log -
a gesture that changed the show without a command writes no record, so `wfg replay` of that session
would not reproduce it. A divergent replay is the alarm, and it is the same alarm every other part
of this engine already rings. The client's own tests help too: a fake link and assertions on the
BYTES a gesture produces (§14.16), which a direct call would not produce at all.

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
| `/godot/bus/<id>/name` | `s` | rw | show | user-authored, what a dropdown shows |
| `/godot/bus/<id>/kind` | `s` | rw | show | `direct` \| `mix` — a direct out, or a mix channel cues send into |
| `/godot/bus/<id>/firstChannel` | `i` | ro | show | hardware output index, 0-based; maintained by the `bus.*` commands |
| `/godot/bus/<id>/width` | `i` | ro | show | explicit, never inferred (§3.9b); changed by `bus.width` |
| `/godot/audio/patchSettled` | `T` | rw | **state** | whether the output patch has stopped following the output list |
| `/godot/audio/device` | `s` | ro | none | the open device's name |
| `/godot/audio/outputs` | `i` | ro | none | hardware outputs the device presents; a cue wider than this is refused at load |
| `/godot/audio/status` | `s` | ro | none | `stopped` \| `running` \| `noClock` — "no clock" and "no interface" are different failures (§6.2) |
| `/godot/engine/launchLatencyTicks` | `i` | ro | none | `1 + ceil (blockSize / samplesPerTick)`, see §11.5 |
| `/godot/engine/rtViolations` | `i` | ro | none | allocations counted inside Go.dot's audio scopes since start |

A **bus** is a summing point — a named, contiguous range of hardware outputs with a declared
width. Processor *slots* (exclusive, allocated) are Phase 4 and are not drawn here.

**The addresses above are `/godot/bus/<id>/…`, not `/godot/audio/bus/…`** — corrected in place
2026-09-21, having been wrong here since §11 was written. A bus is an identified object, so it
publishes under its owner word like every other one; `ShowDocument::addressOwnerFor` is where the
segment is decided, and `ParameterTree` has always emitted it this way.

Implementation update (2026-09-21) — **the output layout, and the patch that follows it.**

A show's outputs are a LIST somebody wrote: so many mono direct outs, so many stereo mix
channels, interleaved however the rig is wired. PRD §6.2's own example is thirty-two mono direct
outs interleaved with sixteen stereo buses on a sixty-four channel interface, and the author's
decision (2026-09-21) is that it is **one list of mixed kinds**, not two lists — because a list
that can be interleaved comes out one-for-one with no patching at all, which two lists cannot.

`Bus/@firstChannel` is therefore not a number anybody types. It is the running sum of the widths
before it, and four commands keep it so:

| Command | Arguments | What it does |
|---|---|---|
| `bus.create` | `kind:s, width:i, index:i, [id:s]` | adds an output; `index` is a position in the list, -1 appends; named "Direct 3" / "Mix 2" |
| `bus.delete` | `bus:s` | takes it away, with every `Route` that named it and clearing every `Slot` that fed from it |
| `bus.move` | `bus:s, index:i` | a position in the list AS IT STANDS, as `object.move`'s is |
| `bus.width` | `bus:s, width:i` | 1 mono, 2 stereo, wider for a processor send |

Each repacks every channel and puts the document's own children into the same order, so list
order, channel order and document order agree from the first command onward. `firstChannel` and
`width` stay `ro` at the door: a client that could write one could leave two outputs summing onto
the same interface channel, and nobody would hear it until the night. `ShowDocument::writeOwned`
is the one private door that writes them, and `document/OutputLayout.{h,cpp}` is the pure rule —
the `Sequence.h` / `FadePoints.h` shape, unit-tested in `tests/OutputLayoutTests.cpp`.

**And the patch follows the list until the show has been heard.** `audio/@outputPatch` is empty
in a fresh show, which the device layer already reads as identity, so adding a stereo mix at the
top of the list moves every output below it — exactly what a designer arranging a rig wants. It
stops following when **`audio/@patchSettled`** goes true, which happens two ways: the settings
window sends it **before the first hand edit of the output matrix lands** (spatcore's
`onBeforeUserPatchEdit`, the hook WFS-DIY latches on — merely LOOKING at the patch must leave it
following), and the Runner sends it beside the first `run.started` of a media run that launches
with audio. From then on each edit MOVES rows instead: a new output takes the next interface
channels past everything in use, a deleted one drops its block, a moved one carries its block,
a widened one appends and a narrowed one drops.

A layout that arrives **unpacked** counts as settled too, and that is the case worth knowing.
`tests/fixtures/bundles/slots` feeds a processor from channel 8 and a foldback from 0, with a
hole between them. Those channels are a rig, not a consequence of an order, so the first layout
command materialises them into the patch before repacking — the processor keeps 8–19 and the list
becomes packed. `validate()` warns about the gap (and about an overlap, which is the one that
matters) rather than refusing the file: yesterday's saved show has to open tomorrow.

The flag is **state, not show**. It records what has happened to this rig rather than a decision
about what the show plays, so it lands in `state.xml` beside the standby and the folds, costs no
undo entry, marks nothing unsaved, and is allowed while the show is locked — which it will be,
because a locked show is exactly the one being played. The author's decision (2026-09-21) is that
it is **kept with the show**: a rig sound-checked on Tuesday must not be re-patched by an edit to
the list on Wednesday.

The window gains an **Outputs** tab between Interface and the two patches: one list with a grip,
an editable name, the kind word, a Mono/Stereo cell, a delete cross, two add buttons, and a
sentence saying which regime is in force (WFS-DIY prints a drag hint per regime, for the same
reason — the two look identical and behave completely differently). The patch matrix's rows are
named after the outputs ("Main L/R · L") rather than numbered, and its row count is the layout's.
`model/OutputList.{h,cpp}` is the std-only reading behind all of it.

Implementation update (2026-09-20): Show → Audio settings stores interface and
input/output patches in the show, with an optional default for new shows.
Windows includes ASIO. Applying while stopped restarts the interface safely.

The output patch's Test mode uses WFS-DIY's shared generator: pink noise, a
20–20000 Hz tone, logarithmic sweep, and its repeating pulse. All start with a
500 ms ramp; level is -92 to 0 dB, initially -40 dB. Hold latches one hardware
output. Leaving the page, closing the window, applying settings, or global
stop/panic clears the test. Tests replace samples on that hardware output;
cue routing and the other outputs continue unchanged.

`audio.testSignal(type:i, channel:i, frequency:i, level:d, hold:T)` changes the
runtime test configuration. Types are 0=off, 1=pink, 2=tone, 3=sweep, 4=pulse;
channel is zero-based, or -1 to stop. `audio.testStop()` clears type, target and
Hold. Read-only `/godot/audio/testType`, `testChannel`, `testFrequency`,
`testLevel`, and `testHold` publish the configuration. These values are not
saved in shows or defaults.

Implementation update (2026-09-21) — **the interface going away mid-show.** Part
of §6.2's "asynchronous failure mode with no defined behaviour yet", and the
behaviour is now **pause, do not stop.** A device that stops delivering
callbacks, reports an error, or comes back describing itself differently puts
`/godot/audio/status` at `noClock`. The show tick freezes where it stood, every
run keeps its position, and **no footer runs** — an outage is not one of §4.4's
three stops, and nothing about it is a thing anybody declared.

While paused the control plane stays up: the engine goes on serving commands and
publishing snapshots at the frozen tick, so a client can still read the show and
be told why it is still. What it may not do is change it. Every command is
refused with `audio-reconnecting` except the ones an outage needs — the four stop
and kill verbs, `audio.testStop`, the document's save and autosave path, the
engine's own `audio.armed` and `run.failed` bookkeeping, and the two commands
below.

Recovery insists on **the same hardware**: interface name, device type, sample
rate, block size and both channel layouts must match what was granted at the
open. Reopening is retried once a second, and a reopened device is then watched
**silently** — its callbacks run and clear their outputs, while the playback
graph and the show clock do not advance — until it has delivered at least three
callbacks and held steady for 250 ms. A 500 ms stall puts it back to unproven, so
a device that flaps never passes rather than handing the show back and forth.

`audio.reconnect()` closes the interface and starts that cycle by hand, keeping
the paused cues: the remedy for a device that is present and wrong.
`audio.connection(ready:T)` is the engine's own, submitted by its watchdog —
`false` when the outage is seen, `true` once validation has passed. The `true`
form is refused with `audio-not-ready` if it has not, so a resume can never
outrun the clock it is waiting for. On resume the graph continues from where it
paused, on the launch handles it still held, and the second-resolution clock
triggers missed during the outage are dropped rather than all fired at once.

A device that returns at a **different sample rate** is held at `noClock` and
retried, and that is where the code stops rather than where the design does.
§6.2's 2026-09-21 amendment decides it the other way: a rate change is a **stop**,
not a pause, and Go.dot then **adapts by resampling** — pitch and duration
preserved. Neither the stop nor the resampling is built, so today a moved clock
domain is a permanent `noClock` with the retry running behind it. That gap is the
next thing this area owes, and it is a gap against a decision rather than an open
question.

The other debt is operator-facing: `noClock` reaches a client through
`/godot/audio/status` and a line of text, which §6.2 says is not enough. The state
has to be visible as a state — and, per §4.8, not by colour alone.

### 11.2 Cue kinds

`kind` grows to `memo | group | media | fade | stop | osc`. Each kind's attributes are nodes
under `/godot/cue/<id>/`, `rw`, `persist = show`:

| Kind | Attributes (type, default) |
|---|---|
| `media` | `file` string, bundle-relative under `media/`; `level` double dB (0, −120..12); `startOffset` double s (0); `Route*` children: `bus` id, `gains` = `C_in × width` doubles, row-major (`/godot/cue/<id>/route/<busId>/gains`, the first list-typed node) |
| `fade` | `target` cue id; `level` double dB; `duration` double s; `curve` enum `linear \| sCurve`; `stopWhenDone` bool (false) *(2026-09-18: arriving is stopping, the stop cue's own fade path)* |
| `stop` | `target` cue id; `verb` enum `hard \| fade`; `duration`; `curve` |
| `start` | `target` cue id *(2026-09-19)*: a memo that presses a button - fired, its run is done the next tick and the target is fired BY NAME as `cue.fire` fires it, standby untouched, through a `cue.fire` record the next tick's hook submits (a replay takes the record). What the live recorder writes into a take |
| `osc` | `address` string, a mounted node; `value` string, one typed atom as the log writes it (`f:0.5`, `s:"…"`, `T`); `wait` enum `none \| sent \| verified`; `timeout` double s |

**Built in two steps: `none \| sent` in PR 2.5, `verified` and its `timeout` in PR 2.6.** The
enum grew only when the engine could honour the new word, because a grammar that accepted one
it ignored would be a show that looked like it was checking and was not.

A media cue's `level` is what was decided. The level a running instance is actually at is
`/godot/run/<id>/level` (§11.3), which is what a fade writes. The two never merge (§4.10).
A missing media file is reported at load and fails the arm, never the load.

**A media cue's output side, added 2026-09-22.** Four more rows on the cue and one more child
element, all `rw` and `persist = show` except where said:

| Node | Type | Meaning |
|---|---|---|
| `/godot/cue/<id>/channels` | `i`, 0, `0..512` | how many channels the file has, decided at import and written into the cue. The file travels between machines and may be absent tonight, so the routing is shaped by this rather than by the disk. Nought is "nobody has said" |
| `/godot/cue/<id>/stereoToMono` | `T`, false | fold a two-channel file to one, each side at half, so a stereo recording can play out of a mono direct out. Ignored unless `channels` is 2 |
| `/godot/cue/<id>/directOut` | `s`, "", refers `bus` | the direct out this cue's channels land on. Empty routes it nowhere by this road |
| `/godot/cue/<id>/sharedOut` | `T`, false | this cue is MEANT to share that out with another (§3.9c). Silences that pair's overlap warning and nothing else |
| `/godot/send/<id>/bus` | `s`, refers `bus` | the mix channel a `Send` child feeds. One per bus per cue; a second naming the same one is refused at `send.create` |
| `/godot/send/<id>/level` | `d`, 0, `−120..12` dB | how loud this cue arrives there. −120 is silence and contributes no coefficient at all |
| `/godot/send/<id>/on` | `T`, true | whether the send is in the mix. Off contributes nothing and keeps the level, so on brings back the same send — the press of its rotary on a surface's Send page (author, 2026-09-25) |
| `/godot/send/<id>/cue` | `s`, `r` | the cue it belongs to, derived from where it sits. As `feed/cue` |

**A DIRECT OUT IS AN ATTRIBUTE AND A SEND IS A CHILD**, and the asymmetry is the shape of the
thing rather than a preference: a cue lands on one direct out or none, so that is a word on the
cue; it sends into as many mix channels as it likes and each at its own level, so those are
identified objects, deleted by `object.delete` like every other.

**And both are below the cue's level, which is what makes the fade a DCA.** `CueMatrix` sums
every coefficient and multiplies the sum by `run/<id>/level`, once, after all of them - so a fade
on the cue moves the direct out and every send together (author, 2026-09-22: *"there is a general
level for the file and a send level for each mix channel. The fades operate as a DCA on top of
this"*). Nothing in the graph had to be added for that; it is where the level already was.

**Heard while it is playing.** A send level or a direct out changed on a cue that is sounding
reaches the matrix on the next tick, through a door that writes coefficients and touches neither
the level nor the smoothers - an arm snaps them, which is right while a voice is silent and wrong
in the middle of a fade. Gated on `showRevision`, so a tick with nothing edited costs one
comparison.

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
| `run.stopAll` | `/godot/cmd/run/stopAll` | — | *(2026-09-18)* §4.4's **Esc**: `run.stop hard` applied to every root run, so members come down in order and every footer runs. An empty table is applied and does nothing |
| `run.killAll` | `/godot/cmd/run/killAll` | — | *(2026-09-18)* §4.4's **double Esc**: `run.kill` applied to every root run; no footer runs. Which of the two a press means is the client's reading of a hand, and each reading is one of these two records |
| `run.solo` | `/godot/cmd/run/solo` | `s` run, `[T on]` | *(2026-09-25, at the author's direction)* a sampler clip soloed on its strip: while it holds, a press on any other strip of its bank - a touch, a pad, a fire by name - is applied and starts nothing. Without `on` it toggles, and the value it came to is what is logged. It lets go by itself when the clip stops (its end, a stop, a kill, a release); a clip that has stopped takes none; a run that is no sampler clip: `bad-value`. The SOLO button of a Mackie strip sends it |
| `run.seek` | `/godot/cmd/run/seek` | `s` run, `d` seconds, `[s made…]` | *(2026-09-18)* a scrub settling: a **media** run is moved to that second of its file - the voice stopped and asked for again on the same track, at the level a fade had brought it to, the run keeping its identifier - and a **group** run is re-seated at that second of its own timeline under the same group run, its members built again from the solver's answer for the scene at that second (over, sounding at their offset, or waiting for their due tick), which is what brings a member already over back. Nothing beside the group is touched. The identifiers a group seek draws ride on the applied arguments as a jump's do. A ranged media run lands at the start of the range holding the second. A fade, a wait, a message: `bad-value`; a run that is over: applied and nothing |
| `record.start` | `/godot/cmd/record/start` | — | *(2026-09-19)* the live recorder on: from now every applied `go`, `cue.fire` and `trigger.fire` on any list is kept with its tick, unbounded, beside the sixty-four-step history |
| `record.stop` | `/godot/cmd/record/stop` | `[s made…]` | *(2026-09-19)* the live recorder off, and what it kept written into a take: a **timeline** group *Take N* in a list named *Live recorder* (made the first time, found by name after), one `start` cue per step with `preWait` the second it was pressed and `target` the cue. Every identifier drawn rides on the applied arguments in order. Refused `bad-value` when nothing is recording, `locked` under the lock |
| `trigger.fire` | `/godot/cmd/trigger/fire` | `s` trigger, `[s run]` | what a matched trigger submits (§12.8); fires the trigger's cue as `cue.fire` does and never moves standby or focus |
| `go`, `cue.fire`, `audio.arm` | unchanged | | `audio.arm` stays the explicit form and still accepts only media |

**Graceful and immediate, drawn now for §4.4 later.** A **stop cue** (any verb) aimed at a group
stops its live members per the verb, **then runs the footer**, then the group reports done — the
same path as normal completion, entered early. **`run.kill`** on a group run kills every
descendant and runs no footer. Esc and double-Esc in Phase 10 are these two paths bound to keys.
*(Bound early, 2026-09-18, at the author's asking — "Panic is missing and Esc key is not bound":
`run.stopAll` and `run.killAll` are those two paths over every root run, and both clients bind Esc
and a second Esc within 750 ms to them; the desktop has a PANIC button beside GO as well. Go Doh!
stays deferred, as §4.4 itself says.)*

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
`standby.set` on a cue of this list that is not a place the pointer may stand is refused with a new
atom. *(That invariant widens once more on 2026-09-16 and the atom is renamed with it; the note
below this subsection is the whole of it, and the sentence that follows is the rule as it stands.)*
**`not-a-stop`** — the cue exists, it belongs to this list, and it is not one of the list's stops:
a header cue, a footer cue, or a cue of the persistent section (§13.11). Those three are a group's
own preparation, its release and a list's standing assertion rather than rows an operator steps
through, and `stops()` leaves all three out by construction, so nothing had to be taught to refuse
them. `not-in-list` keeps its meanings — the cue belongs to another list, or there is no list at
all — and the two stay separate atoms because they send somebody somewhere different.
`standby.set` also stops refusing media, fade, stop and osc cues, which it has done since Phase 2
by accepting only elements named `Cue` or `Group` — a bug no fixture exercised, because every one
parks on a memo or restores standby from `state.xml`.

GO on a member whose manual group has **no live run** — the pointer was placed inside by
`standby.set` or restored from `state.xml` — creates the group run, runs the header, and the member
follows the header: one GO, nothing skipped. **A manual sequence group is reachable only through
GO from standby**: `cue.fire` or a trigger aimed at one is rejected, **`needs-go`**, because
there is nobody to be its parent, and `wfg validate` warns about a manual group nested under a
timeline or auto group for the same reason.

What descent changes that already exists, so PR 3.4 replaces rather than discovers it: decision
C's two tests, the recorded session in `CueListTests` that asserts `standby.set` on a nested cue
is *refused*, and every fixture that carries a `<Group>` — all manual sequence groups by default.

*What the author asked for while looking, and the two rules it becomes (2026-09-16).* **The
pointer may stand inside every group** — decision X in §9, with PRD §3.5 amended to carry it. The
question arrived with the page open and the arrows under their hand: *"up and down stand by as
well as the up and down keyboard arrows move the standby pointer to the next group or individual
cue. However I can't select a cue within a group individually to start from this level, acting on
the following cues. Even start all cues timelines should move the standby pointer from one cue to
the next to try each individual cues it contains."* Asked directly whether that meant every group
or only the manual ones, they answered **every group** — and, in the same breath, that GO on a
member fires **that one cue**, with starting the group from that member left to a second named
gesture this round does not build — the *(proposed)* block at the end of this note.

**What was measured on a live engine before any of this was written**, because most of the walk
the author was asking for already existed and the interesting question was which half. A manual
sequence group's members already walk under ▲/▼ — verified against a running engine rather than
read out of this document. An automatic group is one stop, and parking inside it is refused. A
timeline group is the same. Header, footer and persistent cues are never on the path and never
were, deliberately. So the ask is not a new traversal: it is the same traversal let into two more
kinds of container, and all the work is in saying exactly where.

**Rule one — the pointer may be FOUND, and therefore parked, anywhere it is allowed to be.**
`findOnPath` descends into every group rather than only into manual sequences, and that is the
whole of the change to residence: any cue of this list that is not in a header, a footer or a
persistent section is a place the pointer may be. The walk then follows for nothing. `stepFrom`
already steps through `stops (parent)` and climbs out at either end, so once the pointer is
*inside* an automatic or a timeline group it walks that group's members and leaves by its ends —
which is the author's *"move the standby pointer from one cue to the next to try each individual
cue it contains"*, arrived at by widening one predicate rather than by writing a second cursor.
The sections stay out by construction: `stops()` returns a container's cue children, and `Header`,
`Footer` and `Persistent` are containers rather than cues, so a walk that descends into every
group still descends into no section — which is the same thing §13.11 found from the other side
when the persistent section needed no cursor code at all.

**Rule two — stepping ONTO a group from outside is unchanged**, and this is why there are two
rules here rather than one. `descendTo` and `descendToLast` keep descending only into manual
sequences. They are applied when the walk steps onto a cue, so under a walk that descended
everywhere a reader running ▼ down a list would land on an automatic group's FIRST MEMBER instead
of on the group's own row, and the GO that was meant to fire the scene would fire one cue of it.
That is not a new behaviour badly chosen; it is every show already written changing meaning in
silence, and being found out during a performance. Residence and descent are two questions —
where a pointer may be PUT, and what a STEP lands on — and the author's answer is to the first.
Decision M stands untouched: a GO at an automatic or a timeline group's row fires the scene and
leaves the pointer positionally after it (§3.5).

**So nothing in an existing show changes.** A list walked top to bottom stops on the rows it
stopped on yesterday. What is new is that `standby.set` on a member of an automatic or a timeline
group is no longer refused, and that ▲/▼ *from there* walks that group — a position the operator
now has to ask for, by picking the member and standing on it, which is the shape the author
described: start from this level, acting on the cues that follow.

**Only GO ever writes the standby, so the reason the old rule recorded was never a race.**
`reason::notManualPath` explained itself as *"that cue is one the MACHINE advances (§3.5 — only GO
moves standby, and a pointer the scheduler also moved would be two things moving one pointer)"*.
The second half of that is not what this engine does: the runner advances runs and writes
`list/@standby` nowhere, which is the rule §14.7's own column states command by command and the
Phase 4 black-box driver asserts by reading a second list's pointer across a command and finding
it unchanged. What the refusal was really protecting was the OPERATOR'S MODEL — a pointer sitting
inside a chain the machine is stepping through *looks* like a pointer about to be overtaken, and
it never would have been. That is a judgement about their own dark booth and it is theirs to make;
they have now made it the other way. Nothing had to be made safe for it, which is worth saying
plainly rather than leaving a reader to wonder what was given up: the widening costs one predicate
and no new invariant.

**The atom is renamed because what it refuses is no longer what it says.** `not-manual-path` was a
sentence about nesting — *the pointer cannot be the parent of something the machine parents* — and
nesting is precisely what has stopped being refused. What is left is a section question, so the
word becomes **`not-a-stop`**: the cue is in this list, and it is not one of the list's stops. A
reason code is part of the log format and therefore a contract (`command/Command.h:107-109`), so
the rename is a change to that contract and is recorded here rather than quietly made — a log
written before today carries `not-manual-path`, and what it meant by it is the sentence above. The
predicate both doors ask goes the same way and for the same reason: `isOnManualPath` becomes
`mayStandOn`, named for the QUESTION rather than for an answer that has changed once and may
change again.

**Two edges the widening leaves exactly where they are, named here rather than fixed.** Stepping
out of a non-manual group and back into it is not a round trip. From its first member ▲ finds
nothing before it among the group's stops, climbs out, and takes the group's previous sibling;
▼ from there lands on the group's own ROW rather than back on the member, because `descendTo` does
not enter a group the machine parents. The operator ends one row higher than they started, on the
scene rather than in it, and the members are reachable by `standby.set` and by walking on from one
of them but never by walking in. Both ways of closing that — resting on the row on the way out of
its own members, or refusing to climb out of a group the pointer was deliberately put inside —
change what ▲ and ▼ do somewhere else, and neither is what was asked for in this round, so it
waits for somebody to try it with a show open. And **the manual loop's wrap stays keyed on the
group being a manual sequence**: a pointer parked inside a timeline or an automatic group to try
one cue leaves at the last member like any other stop rather than wrapping into that group's next
round. §3.6's wrap exists to keep the operator inside a scene they are the parent of; holding them
inside one that runs its own rounds unasked would be the machine moving them around, which is the
one thing §3.5 has always refused.

**One predicate widens and the other does not, and they part company on purpose.** `mayStandOn`,
over `findOnPath`, answers *where may the pointer be put*. `ShowWalk::Placed::onManualPath` — a
second predicate, in another file, written for the solver — answers the different
question the solver asks — *where does a jump LEAVE the pointer* — and §13.9's answer to that
stays "after the whole scene" (`cue/Solver.cpp:196-209`). Parking inside a timeline scene is a
decision somebody takes; being put there is the machine choosing for them, and it would leave an
operator standing on the third member of a scene they did not aim at. §13.16's *"§3.5 lets the
pointer sit at the top of a list or inside a manual sequence group and nowhere else"* was one
sentence doing for both questions, and is now true only of the second. Whether a jump should ever
land inside a scene is §13.9's question and the author's; this round does not answer it, and the
field's name will want revisiting on the day it is answered.

**The second gesture: decided to exist, and not built here** *(proposed)*. The author settled that
GO stays *fire the one cue the pointer is on*, and that starting the group from the member the
pointer is on is a gesture of its own. Its name, its arguments and its edges are this document's
proposal and not yet theirs.

- **A group run already holds what "enter at member N" needs.** `round` is the ordered member ids
  for the pass, drawn when the round begins, published at `/godot/run/<id>/round` (§12.2), logged
  as `run.round` and read back on replay (§12.3). Entering at member N is a round that **begins**
  at N — the same list, started at an index — so there is no new run machinery to build, and a
  replay reproduces the entry for free, because the round is the data and a replay never consults
  the RNG.
- **Suggested name `go.from`, no arguments**, acting on the focused list's standby exactly as `go`
  does — so it is reachable as a named command (constraint 11) and a page's key finds it through
  `gestures/commands.json` like every other gesture, rather than being a modifier the engine never
  hears about. With the standby at a list's top level there is no group to start from and
  `go.from` is plain `go`.
- **Open, and each of these is a decision rather than an implementation detail.** What *from here*
  means in a **timeline** group, whose members are simultaneous by construction: starting a
  timeline scene at its fourth member is §3.13's `list.aim` and a load-to-time in all but name, so
  it should either **be** that or be refused, rather than become a third thing that resembles both.
  Where the pointer goes afterwards — out of the group, as decision M leaves it, or onto the member
  after the one entered at. And whether it is refused while the show is locked: `go` is not
  (§14.11 — a lock stops edits, not the show), and `go.from` starts a scene rather than editing
  one, so the same answer looks right and has not been given.

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
| **M12** | **the clip's own wrap wins, at every configuration** | 0 join error at every block size and both rates. Damage energy - summed squared deviation around the join - is smaller for the wrap than for a placed boundary in all ten cells, by 5.5× to 23 000×; at 96 kHz up to 256 frames the wrap has no damaged sample at all. A placed boundary costs a fixed 25–33 samples at 0.49 of an amplitude of 0.5, block-size independent, which is `SlotControlNode`'s own 40-sample stop decay. `setLooping` on a clip armed not-looping never comes back. *Re-measured 2026-09-24 at the Tracktion pin `13b5132`:* the wrap is now **exact in all ten cells** (no damaged sample, worst deviation 1.2e-7), because the resampler no longer carries a stale 2-sample offset across the jump. The placed boundary and `setLooping` are unchanged. |

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

*Amended 2026-09-22 — and a direct out is not an exception to it.* `media/@directOut` and a
`Send` carry no matrix and the engine synthesises their coefficients, which looks like the
inference this paragraph refuses and is not. The difference is what the rule is made of:

- A `Route` or a `Feed` is a matrix because the DESTINATION says nothing about what should go
  where — a source among twelve processor inputs is a decision only a designer can make, and
  defaulting it would be guessing at intent.
- A direct out and a mix channel derive theirs from facts the document **states**: the cue's
  `channels`, the bus's `width`, and `stereoToMono` where a fold was asked for. Channel to
  channel where they match, one channel onto all of them where the cue is mono (which is the
  rule `route.default` already applies), both sides at half where the fold says so — and a
  REFUSAL, `bad-route`, where the cue is wider than the destination and nobody asked for a fold.
  Nothing is inferred; a rule is applied to declared numbers, and the one case a rule cannot
  cover is the one it refuses.

That last clause is what keeps §3.9b's *"refuse silent downmix or upmix"* true. A fold is a
downmix somebody ASKED for, which is a different thing entirely from one that happened.

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

*Amended 2026-09-22 — the third resource kind, and the third answer.* Phase 5 adds **direct outs**
to the same analysis, over the same live ranges: a media cue's `directOut` is a claim exactly as a
`Feed` is, `bus/usage` and `bus/overlaps` publish it exactly as `slot/usage` and `slot/overlaps` do,
and `media/@sharedOut` silences a pair exactly as `feed/@shared` does. A **mix channel is never
analysed** — many cues arriving at one mix is what a mix is for, so nothing about it is a claim.

What is genuinely new is that a second question is asked of the same ranges. The menu that picks a
cue's direct out has to mark each output **free, taken or undecided**, and "overlap or nothing"
cannot say the third. So a claim now carries **two** bounds:

| bound | what it says | an intersection of two is |
|---|---|---|
| `mayLast` | where the release rules say the claim is certainly **back** | POSSIBLE |
| `mustLast` | where it is certainly still **held** | PROVEN |

**The release row is an upper bound, and reading it as a lower one is a lie.** Three finite media
cues in a MANUAL group all release at the group's end, so by `mayLast` the first covers the third —
but the operator held the GO for forty seconds during a scene change and the first finished
thirty-seven seconds ago. What actually proves a claim is still live across a manual boundary is
that the cue **never ends on its own** (`Walk::unbounded`, whose own comment is the rule: *"a cue
that ends on its own is over by the time a later manual step is reached and one that does not is
still going"*). That, plus exact seconds inside a timeline or automatic chain, is the whole of what
can be proven; everything a person's GO separates is undecided.

| Node | Type | Meaning |
|---|---|---|
| `/godot/bus/<id>/usage` | `s` | as `slot/usage`, for a direct out; empty for a mix channel |
| `/godot/bus/<id>/overlaps` | `s` | as `slot/overlaps`; a warning, never a refusal, and never a reason to refuse an assignment |
| `/godot/cue/<id>/outsBusy` | `s` | `<bus> <cue>` pairs: outputs provably carrying another cue while this one plays |
| `/godot/cue/<id>/outsMaybe` | `s` | the same, for outputs that might be |

**The cue-side rows are capped at one pair per output**, naming the *nearest* blocker. Uncapped they
would be quadratic in the show: `tests/fixtures/make_large_show.py` already measures ~300 pairs per
resource on a 500-cue manual list, which cue-keyed is ~125,000 pairs and megabytes of text merged
into every snapshot. The menu has one mark per row and room for one name, so one is what is
published — and the nearest one is the useful one.

**They are the cue-side twin of `usage` and NOT of `overlaps`**, which decides what `sharedOut` does:
it removes the pair from the warnings and leaves the mark standing. Which outputs carry sound is a
fact; the warning is a complaint; a designer who said they meant the sharing answered the complaint
and did not change the fact.

**And a client could not compute any of this.** It has no `advance` row, no walk, and no way to
reach a group's length — that is `ShowWalk`, engine-only. Re-implementing it behind the client
boundary is exactly what `scripts/check-client-boundary.py` exists to prevent, so the engine
publishes the answer and the window reads it.

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

#### What the desktop's load to time changed (2026-09-19): the history is the clock

The author, on seeing scrubbing (§14.16) beside the load to time he was about to ask for: *"1 is like
scrubbing through the load to time history. 4 [a live recorder] is like dumping the load to time
history to a group for replay. We might need to adapt the original design of the load to time
history for this."* The adaptation is that **the solver reads the steps when it can.** `solveHistory`
takes the aim and the list's steps: the aimed cue's most recent step plus the offset is an INSTANT on
the wall clock, and every step at or before it is a cue that had been going for (instant − step)
seconds then — sounding if its material lasts that long, over otherwise, a scene with its members
placed by the same clock through the same `planTarget` the order reading uses; a stop step ends its
target from then on, a fire after a stop is a new run, a fade step's trim is whole past its duration
and proportional inside it, an osc step is a value with the last writer by time. The pointer lands
after the last GO in the instant's past. So a bed started three GOs ago is three GOs of real time in,
which is what the room hears, rather than "over" because it comes earlier in the list, which is what
§3.13's step 1 had to assume of a manual list with no clock. A cue never fired this session has no
step and gets the order reading; the plan says which in `how` (`history` | `order`) and carries the
`instant` (−1 for the order), and `list/solve` publishes both. `solveAim` is the one door: the tree's
`solve` node and `list.loadToTime` both go through it with the list's history.

**And the jump retimes the history.** The steps are on the wall clock, and a jump puts the show where
it was at the instant: from then on every kept step moves forward by (now − instant), so the sound it
describes and the step agree again, and every step after the instant — the ones the jump undid — is
dropped. A second aim after a jump therefore reads right, which it could not have with the wall
ticks left alone (a cue fired after the instant would have read as still sounding). A scene
re-seated by `run.seek` moves its own step to where the second says it was fired. The history is not
a log of the evening any more; it is the list of what is in force, on the clock the show is on —
which is exactly what a live recorder will dump into a group.

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
footer, `standby.set` refuses its cues with `not-a-stop` (`not-manual-path` until 2026-09-16,
§12.6), and they are not GO targets.

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
to learn that it was not. *(The atom is `not-a-stop` from 2026-09-16 and the sentence survives the
rename intact: `findOnPath` now descends into every group, and a section is not a group — `stops()`
returns cue children, and `Persistent` is a container. §12.6.)*

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
fallback rather than on a decision nobody has taken. *(Dated note, 2026-09-17: E is now settled —
the author was asked directly, before a line of the client was compiled, and chose IN PROCESS -
with the desktop client given as much slack as it needs, and one line kept: the show changes only
through named commands. The reasoning above is
left standing because it was the reason the question survived to be asked deliberately rather than
answered by accident, which is exactly what it was for. §9's E and §14.16 carry the answer.)* Everything Half B could draw is a node or a
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
`Cache-Control: max-age=31536000, immutable` (§14.5). *Corrected by PR 5.8 (2026-09-14): the key
is the audio, and the answer also depends on the analysis — a moved ramp stop bumps
`timbre::formatVersion` and rebuilds the pyramid under the same name — so the route says
`no-cache`, and a page keeps what it fetched in memory for its own life instead (§14.5's note).*
Anything reached by an address under
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

*That paragraph describes the page of 2026-09-10, and four of its clauses are now false
(2026-09-16).* The page sends **twenty-one** named commands and the inspector's `node.set`, not
fifteen: the six the gesture table gained with Half A's nodes are `undo`, `redo`,
`document.save`, `document.revert`, `document.recover` and `document.discardRecovery`
(`clients/console/gestures/commands.json`). `document.save` is sent from a button and from
Ctrl/⌘-S, undo and redo from buttons that carry the transaction's own name and from Ctrl/⌘-Z and
Ctrl/⌘-Shift-Z, and the lock is written as well as read, from a button in the transport. And the
page has colour: a playing media cue's timbre is on its run row as three numbers in words
(§14.12's rule kept — the words are the carrier, not the colour). What is still true is the
operator half this section was written to name: no run pointer on a cue-list row, no
`run.advance`, `run.prune`, `run.unprune` or `run.stop`, no round pills, no range on the strip,
no bulk edit, no header pane, no curve editor, no display presets and no show mode. Those are
5.11 to 5.17, and they are the phase's second half rather than a debt.

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

*Corrected by PR 5.9 (2026-09-15), from what its instrument saw rather than what this paragraph
predicted.* **The scroll did not snap, in Chromium.** The pane was emptied and refilled with
nothing laid out in between, and the rows came back the same height, so Edge kept `scrollTop`
in every trial — by script, by the wheel, and with the standby moving on every poll; Firefox and
Safari were not checked. What the wholesale `innerHTML` did cost was two other things. Every
row was rebuilt and laid out ten times a second. And **a click whose press and release fell
either side of a poll was lost**, because the two landed on different elements and the browser
fires no click at all: a *kill* pressed on a playing cue did nothing, which was reproduced. The
slider guard was exactly as described: Space after touching it never reached GO. And M24 found a
**third blocker this paragraph did not name**: the page started a poll every hundred
milliseconds whether or not the last had answered, and against a Debug engine serving the
500-cue tree it queued some 1350 requests and drew once in four minutes, or never. So 5.9 keys
the rows *and patches a changed row in place*, keeping its elements, a kill button's included,
across polls; builds the trigger index once per poll, which turned out to be nearly all the time
(§14.14's M24); narrows the guard, gives Escape a meaning, and lets a checkbox keep its Space and
a menu picked with the mouse let go of the keyboard; blurs the slider on a pointer release but
not on a key step, so a slider reached with Tab keeps stepping; and chains the polls, one in
flight.

*What PR 5.10 built (2026-09-15).* **The split, with behaviour unchanged and measured to be.**
M24 after it reads 6.7 ms for `render()`, against 6.3–6.8 ms before it, and every behaviour
5.9's instrument checks — the scroll, Space from a focused slider, the arrow, a focused button,
Escape — reads the same. The files as built:

- **`plumbing/`**: `osc.js`, `link.js`, `poll.js` and `tree.js`.
- **`model/`**: `index.js`, which adds `triggersOf` and `overlaps` to the tree object so every
  view still asks the tree, and `selection.js`. No `layout.js` yet: it is 5.14's, and it lands
  with the presets it holds.
- **`views/`**: `didi.js`, `gogo.js`, `inspector.js`, `aim.js`, `strip.js` and `transport.js`.
  With them `common.js`, the text every view says the same way; `reconcile.js`, the keyed rows;
  and `view.js`, described below. The header and the curve are 5.13's and 5.16b's.
- **`gestures/`**: `clicks.js`, `keys.js`, `fields.js` and `table.js`, beside `commands.json`.
  `fields.js` holds the inspector's commit-and-Escape rules and the pointer state the key
  handler shares.
- **`styles.css`** and **`app.js`**, and **`index.html`** as the shell.

Three things the split had to decide:

- **The shell addresses `/ui/app.js` and `/ui/styles.css`, not `./app.js`.** The engine answers
  `/ui` and `/ui/` with the same page, and a path relative to `/ui` would look for the modules at
  the root of the server. That is the one address the instrument, and anybody typing it, uses.
  Imports inside the modules stay relative, since they resolve against the module.
- **A classic script in the shell says what a module cannot.** A browser refuses to load modules
  for a page opened from disk, so "this has to be served by the engine" is said by the shell. The
  same script catches a module that failed to load, which is the one thing an edit can break on a
  page with no build step. The browser says why only in its console, so the page points there.
- **`view.js` is a seam, not ceremony.** A module's names are not the window's, so the M24
  instrument could no longer wrap `render` by its global name. Every render now goes through a
  `view` object that `app.js` fills, published with the tree as `window.goDot`, and that is where
  the instrument wraps them.

**`gestures/commands.json` is the table §14.3 drew, read by the page.** It has three parts:
`keys`, each key and its command; `gestures`, each click's command with the OSC type tags it
sends; and `buttons`, the names the transport's buttons carry in their own `data-cmd`. Every key
and click finds its command's name through the table, and no key or click handler spells one.
**The check 5.18 was to add landed with the table.** `client_page.py` walks the module graph as a
browser would and refuses an orphaned module. It holds every entry to `wfg commands`, both the
name and whether the gesture's arguments fit the signature, and holds the shell's `data-cmd` list
to the table's. With a command misspelt or an argument given the wrong type, it fails, which was
tried.

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
| **5.15** | save, recovery and undo gestures | landed line by line as Half A lands each node, then consolidated here into `views/transport.js` and `gestures/keys.js`: Ctrl/Cmd-S, Ctrl/Cmd-Z, the transaction word beside the button, the recovery banner's *recover* and *discard*. The delete confirm goes away, because the page's own comment says why it is there — *"Undo is Phase 5's and does not exist yet"* (`index.html:1609-1611`) — and by then it does. *Nothing is left of it (2026-09-15).* Each gesture landed with its node in 5.2–5.5, the delete confirm went in 5.4, and 5.10's split put them in `views/strip.js`, `views/transport.js` and `gestures/keys.js`. The one confirm left is revert's, which 5.5 asked for |
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

*What PR 5.18 built (2026-09-15).* **`node --test` over the pure modules, where Node is new
enough to run them.** The tests are `tests/console/*.test.mjs`. They live in the test tree
rather than in `clients/console/tests/` as the plan had it, so the folder the engine serves
stays the page and nothing else, and `client_page.py`'s orphan check has nothing to exempt. They
import the page's own files as the browser does, with no copy, no build step and no
`package.json`. Three files, 29 cases:

- **`osc.test.mjs`** holds the encoder to the packets `OscCodecTests.cpp` builds by hand: the
  specification's 440, the padding rule, every tag the page sends, two's complement, UTF-8, and
  the aim's double beside the float it would otherwise be, both spelled out by hand. It also
  checks that a list goes as one string, which is §14.6's contract with the door.
- **`reconcile.test.mjs`** counts the reconciler's own promises. An unchanged row costs nothing
  at all. A changed one keeps its element and its buttons, and only the difference is written.
  One row moved costs one move whichever way it went, and a block moved costs one move per row
  in it. A row leaving costs one removal. A key wanted twice is drawn twice. `morph`'s rules are
  checked too, the focused field among them. The count is taken on `fake-dom.mjs`, the least DOM
  `reconcile.js` touches. *This corrects §14.15,* which called the reconciler "a pure function
  over a `Map<id, element>`". It is not one: it works on the DOM, and reads what is on screen
  from the screen. So it is tested on a DOM small enough to count what changed, and what changed
  is the one thing about it a browser would not say.
- **`model.test.mjs`** covers `flatten`, the tree's questions, the trigger index filed by reply
  (a new reply is a new index), overlaps told to both cues of a pair, and `esc` and `seconds`.

Each file was tried against a mutation of the code it covers. The padding rule, the
reconciler's step past a carried row, and an index keyed by something other than the reply were
each broken in turn, and each broke its file.

**Registered as `console.unit` only where it can pass.** `find_program (WFG_NODE node)` comes
first, then the version. The page's files are `.js` ES modules with nothing beside them to say
so. Node reads such a file as a module from 22.7, or from 20.19 on the 20 line. An older Node,
or none at all, gets a STATUS line and no entry. There is one entry and not one per locale,
since nothing here formats a number for a person. `intersect`/`agree` and `pointsToText` belong
to 5.12 and 5.16b, which will add their tests with the code; the harness is ready for them.

*What the author changed while looking (2026-09-16).* **The page went in front of the author for
the first time, and the commits below are their notes rather than the plan's.** They are recorded
here because §14.3 is where this client's design lives, and because several of them decide
things the remaining views inherit.

- **Type and contrast.** Every font size reads one knob, `--type`, at **1.25** — *"enlarge font
  by 25% more or less"* — with the original sizes kept as named tokens so the scale the page was
  drawn at stays legible. The two greys are lifted to about 8:1 and 5:1 against the panel, from
  4.8:1 and 2.1:1: *"the darker grey text is too dark to read on a black background"*. A third
  grey, `--ink-off`, is what a disabled control wears, since lifting the faint one alone would
  have made every dead button look live.
- **Didi grew three columns** — `preWait`, `duration`, `postWait`, editable where the row is
  (*"when each field is applicable"*, which the TREE answers: a memo has no duration node, a
  media cue's is the file's length and read-only, a fade's is a decision). A nought shows as an
  empty box, and clearing one commits a nought: the display rule and the commit rule are one
  pair in `views/values.js`, because the first without the second taught a gesture the engine
  refused.
- **Gogo says `playing` and `armed` as marks** — ▶ and ○ — *"can be an icon or just the yellow
  mark"*. Two SHAPES rather than two colours, which is what keeps §4.8: the word stays as the
  title and the label, and every other state keeps its word outright.
- **The inspector hides what the engine says back** behind a `details` fold — *"hide the internal
  stuff like the various UIDs, hash and other things"* — split by `ACCESS` rather than by a list
  of names, so a row that becomes writable moves by itself. The identifier is the first line
  inside it.
- **And it reads in the order somebody works**: what it is, `when` (preWait, duration, postWait —
  *"something logical time wise"*), the kind's own rows, how it sits in the list. Keyed on the
  cue's KIND, not the owner word: every attribute a cue carries publishes under
  `/godot/cue/<id>/`, whatever owner the table files it under. A group's `mode`, `advance` and
  `selection` sit together, which was the author's own example: *"so the user will set one after
  the next and not hunt for the next thing much further down the list"*.
- **The inspector is the middle pane, and only when something is picked.** *"It means a lot of
  back and forth from left to right selecting and adjusting... QLab has the Inspector at the
  bottom"*. Between the two panes, the pick-to-field distance is one pane rather than two, and
  Gogo — a readout — stops sitting between the list and the fields. With nothing picked the pane
  closes and the cue list takes the width. **The foot arrangement is on a button beside it**,
  where the block headings sit beside their fields rather than over them, and it is where the
  bottom editor panel belongs when there is something wide to draw: 5.16b's curve, 5.17's bar,
  video after that. The author's shape for it: *"when we need to display something like a
  timeline or a waveform/video we open a panel at the bottom of the screen"*.
- **The panes move rather than jump**, 170 ms of it, *"since we have very similar panels"* —
  with the inspector also carrying the selection's own colour on its heading and its leading
  edge, so it is told apart after the movement as well as during it. `prefers-reduced-motion`
  turns the movement off and leaves the colour.
- **A section is a frame that shuts** — *"I think we're missing a clearer delimiter between the
  header section and footer, something like a collapsible frame"*. A group's header, its footer
  and a list's persistent band each get a head row (a twist, the word, a count), a rule down the
  left of every row the section holds, and an end row closing it. **Drawn as a rail on each row
  rather than as a box around them**, because the reconciler keeps ONE FLAT LIST of keyed rows:
  a wrapper element would be a second level of nesting it does not model, and every row inside
  it would have to be reconciled against the wrapper rather than against the pane. The end row
  is pushed after the lines rather than worked out from a count, because a header cue may itself
  be an open group whose own children follow it. And the frame travels DOWN the recursion: a
  group sitting in a header has role `header`, but its members have role `member` and are still
  drawn between that header's two band rows, so what a row is drawn INSIDE is the question, not
  what its own `role` says.
- **And what the reader shut outlives the reload** — *"yes we need to store the expanded and
  collapsed state of the different containers"*. `model/remember.js`: one localStorage key,
  `godot.console.view`, holding the shut keys per document path and the panel's two preferences
  globally, capped at twelve documents by recency — with the document being written taken out of
  the eviction's reach, or a machine whose clock ran ahead and was put right would sort tonight's
  show off its own end. It is the reader's and never the engine's (§14.1): a second operator on a
  second tablet folds their own groups with no byte reaching anybody else, which is also why it
  is not a node and needs no row in the table. Every touch of storage is wrapped, because naming
  `localStorage` is itself what throws in a browser told to block site data: a private window or
  a quota refusal leaves the page working exactly as it did before the file existed, with the
  fold lasting as long as the tab. **This is the store 5.14's display presets land in**; it
  arrived early because the author asked for the folds.
- **A header line and the cue it is a view of point at each other** — *"I could get the focus of
  a header item with the actual cue"*. A derived line is the one place on this page where ONE
  OBJECT IS DRAWN TWICE: its own row where it sits in the list, and an italic line in the header
  of the group that gets it ready. Clicking the line now picks the member AND takes the reader to
  the member's own row — whatever must be unfolded is unfolded first (the walk is up `parent` and
  `role`, both published and derived, so it cannot disagree with the tree the rows come from),
  the row is scrolled in with `block: "nearest"`, which does nothing when it is already on screen
  so the list never jumps for no reason, and it is marked for about a second. The mark is IN THE
  MARKUP rather than set on the element afterwards: `morph` copies the fresh element's attributes
  onto the live one and removes any the fresh one lacks, so a class set by hand is wiped by the
  next poll a tenth of a second later. The mark on a member's own row goes the other way and does
  NOT move the selection — looking is not picking — and it is a link only where the header it
  names actually shows the line: `preset` naming a non-ancestor is a validate warning the engine
  tolerates rather than refuses, and a way in to a place the page cannot reach would open that
  group and its header, throwing away the folds the reader now keeps, to show them nothing.
- **And one of the day's notes was not about the page at all.** *"I can't select a cue within a
  group individually to start from this level, acting on the following cues"* — which is the
  standby pointer, and therefore the engine. It is written where the cursor's rules live, in §12.6,
  rather than here, and it is **decision X** in §9: the pointer may now stand on any cue that is
  not in a header, a footer or a persistent section, so ▲/▼ walk an automatic or a timeline group's
  members from inside it; what a step from OUTSIDE lands on is deliberately unchanged, or ▼ down a
  list would land on an automatic group's first member and GO would fire one cue where the operator
  meant the scene. The page inherits all of it without a line of its own — the arrows already send
  `standby.next`/`previous` and the *park* gesture already sends `standby.set`, and both now reach
  further — which is what a gesture table naming commands rather than behaviours is for. The second
  gesture the author asked for in the same breath, *start the group from here*, is `go.from` in
  §12.6 and is *(proposed)*: when it is settled it is one row in `gestures/commands.json` and one
  key.

**Two things the author named for later, neither started.** A **toolbar** — *"there are probably
a tool bar to design too"* — which is a design question about what an operator reaches for
without the keyboard, and belongs beside 5.11's gestures rather than before them. And the
**application's own parameters** (audio device, ports, the display preset) — *"once we are in the
C++ design"*, which is §14.16's client and not this page: the engine has no `/godot/app` and the
browser has no honest place to keep one.

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
| `/godot/cue/<id>/hash` | `s` | ro | none | 1 | the sha256 of this media cue's file, once the analyser has hashed it — the key the route below is addressed by. A media cue's only — a memo, group, fade or stop has no such node *(PR 5.8's reading of owner `media`; this cell said "empty for every kind but media")* — and empty until the hash exists *and* its pyramid does | runtime | 5.8 |

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

*And only at the first arm — a refinement PR 5.6's review added (2026-09-14).* With an audio side
the first arm reserves a track, and `armMedia`'s own guard returns on every later one, so a hosted
run keeps the file it was armed with. Without one the track stays −1 and a run can be armed twice
— a cue with a pre-wait, armed on entry and again when the wait elapses; a sequence member,
spawned and armed and launched minutes later — and a second read would pick up an edit the hosted
session never plays. So the copy is taken only while the run has none, and every configuration
agrees with the one that sounds. The test that pins it was checked the only way that proves a
test: with the guard removed it fails, reading the edited file.

The same double arm reaches `claimSlotsFor`, which PR 5.6 did not touch: on a second no-player arm
`holderOf` answers with the run itself, so a Feed would queue the run behind its own claim and an
Insert would warn `no-channel` against itself. It is suspected from reading, not seen in a test,
and it lives in Phase 4's claim logic, so it is recorded here rather than fixed in passing.
*Confirmed and fixed (2026-09-15).* A test written first, before any change, showed all three:
a cue with a pre-wait, fired with no audio side, ended `pending` on the slot it held, listed as a
waiter for it, and warning `no-channel` against its own rack channel. `claimSlotsFor` now leaves
alone a slot the run already holds or already waits for. The second half matters as well: a run
queued behind another cue and armed twice was queued twice, which the second test found when
the guard was cut back to its first half.

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
| `GET /media/<hash>/timbre?level=N` | that level's frames, exactly the bytes the `.tpy` holds | `application/octet-stream`; `Cache-Control: max-age=31536000, immutable` *(built as `no-cache`: PR 5.8's note below)* | 400 for a hash that is not exactly 64 hex characters *(built: 64 **lower-case** hex, so upper case is 400 too)*, or a level that is not a number; 404 for a hash the snapshot does not hold *(or holds without a pyramid)*, or a level the pyramid does not have |
| `GET /media/<hash>/timbre?INFO` | the header as JSON: `sha256`, `seconds`, `sampleRate`, `window`, `hop`, and a `levels` array of `{ frames, bytes }` *(built with `formatVersion` after `sha256`)* | `application/json`; the same immutable header *(built as `no-cache`)* | as above, minus the level |

`?INFO` exists so that a forty-pixel Gogo bar and a full-width editor waveform each ask for the
level they want in one round trip rather than fetching the finest and throwing most of it away,
which is the whole reason §3.30 asked for a pyramid instead of a frame array.

**`immutable` is the exact opposite of what `/ui` gets, and both are right.** The client
directory answers `Cache-Control: no-store` (`OscQueryServer.cpp:246`) because the page is
edited while the engine runs and a stale module is a bug the author cannot see (decision V,
§14.3). A pyramid is named by the sha256 of its own source, so a given URL can never answer
differently; a year is not optimism about the cache, it is a statement about content addressing.
*Corrected by PR 5.8 (2026-09-14), and the note at the end of this section says why: the name is
the audio's, not the analysis's, so the route answers `no-cache`.* The content address still
does the other half of this paragraph's work unchanged.
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

**What PR 5.8 built (2026-09-14), and what its review changed.** The two rows landed as drawn,
`run,timbre` beside `position` and `media,hash` beside `duration`. `ParameterTree` gains
`setMediaInfo`, takes **one** `snapshot()` per publish and reads every hash and every timbre out
of it, so a record landing mid-publish cannot give one run colours another lacks. The document half
leaves a roster of media cues and their `file` behind, and the runtime half emits
`/godot/cue/<id>/hash` over it: `prepare`'s pair, exactly once per media cue wherever it sits,
list, group, header or persistent section. The hash reads empty until the record has its pyramid as
well as its hash, so no client can read a hash the route would refuse. `timbre` is
`timbre::frameAt` — the finest frame at `floor (position × rate / hop)`, held at both ends — printed
as the hue to a tenth of a degree and the other two to thousandths, fine enough that every byte
still prints differently. A silent frame reads `0 0 0`, a real reading and not the empty "not
analysed yet". The route is a generic hook on the server, `serveRoute (prefix, handler)`, so the
shell stays free of Go.dot, and it sits beside `/ui` exactly where this section put it. The hook
refuses a registration after `start()` (the HTTP thread reads the table without a lock), an empty
or `/` prefix, a prefix ending in `/`, and an empty handler. The server writes `Content-Type`,
`Content-Length` and `Transfer-Encoding` itself, and drops a handler's own copies. The pyramid
answer, `oscquery/TimbreRoute`, checks in the order drawn: shape, then hash, then question, then
records. The console prints the reading in words on a media run's row, and not for a run that
failed before it launched. Four corrections:

- **No year of `immutable`: every pyramid and every `?INFO` is `no-cache`**, and every refusal
  stays `no-store`. The URL names the audio, but the bytes
  also depend on the analysis. A moved ramp stop bumps `timbre::formatVersion`, and the pyramid is
  rebuilt *under the same name* (§14.12). The author will move stops while looking at the bar,
  and a browser keeping a year-old copy would go on showing the old colours, as if the move had
  done nothing. The handler sees no request headers, so an ETag and a 304 were not available either.
  So `no-cache`: a browser may keep a copy but must ask again before reusing it, and a page keeps
  the levels it has fetched in memory for its own life anyway. `?INFO` now carries
  `formatVersion`, second after `sha256`. Immutability can come back with a URL that names the
  version too — once a client has somewhere to learn the version *before* it asks.
- **Every throw is a 500, not only a `std::exception`.** juce_simpleweb already survives a
  `std::exception` from a handler and answers it with nothing. A throw of any other type left
  `io_service::run()`, and JUCE's thread entry swallowed it: the one thread that carries the HTTP
  port *and* the WebSocket ended quietly, every subscription with it, and `stop()` then waited for
  ever on a connection flag nobody would clear. The route's guard catches everything, and its 500
  says `no-store` like every other refusal.
- **The table's cells, as built:** the hash node exists on media cues only; a hash is 64
  *lower-case* hex characters and anything else is 400, upper case included; and 404 also covers a
  hash the analyser holds without a pyramid. The line numbers this section cites in
  `OscQueryServer.cpp` have moved twice since it was drawn; the code's comments now carry the
  reasons.
- **A fix outside the timbre, found while building it.** `collectCue` answered *every* row named
  `duration` from the media table — a fade's and a stop's too, which are lengths somebody decided —
  so every fade and every stop has published a duration of nought whatever the show said, since
  PR 4.1. The branch is now a media cue's only, and a test pins all three kinds.

**Proposed, and the author's (PR 5.8's review, 2026-09-14): a `run,hash` beside `run,timbre`.**
`cue/<id>/hash` follows the file the cue names *now*, while a run plays the file it was armed with
(`Run::media`, taken at the first arm). So once a playing cue's `file` is edited, or a run outlives
its cue, a bar keyed by `cue/<run.cue>/hash` draws one file's colours under the other's playhead,
and it disagrees with `run/<id>/timbre` at that same point — `cue/<id>/duration` already has the
same mismatch, and the console's row shows it. One runtime row — the hash of `Run::media`'s record,
under the same rule that the pyramid must be present — would key a run's bar by what the run
actually plays, and would give the bar its length through `?INFO`. It belongs with 5.17, the first
code that draws a bar. Until then, the `media,hash` description says which file it follows.

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

*What PR 5.16a built (2026-09-15).* **The row, the door, and one rule asked in three places.**
`fade,points` is in the table with an empty default and no range, because its elements alternate
between two ranges and no single column can say both. What a curve *is* lives in one function,
`doc::readFadePoints` (`document/FadePoints.{h,cpp}`), which the write door, `validate()` and the
Runner all ask, so the three cannot drift. The door took lists as this section said it had to:
`Schema::parseList`, the reader's own function, for each element, and the canonical text stored
as a string, which is what the reader stores. A bad element answers `type-mismatch`, as a bad
scalar does. `toText` reads a list back whole. `validate()` had been re-parsing list attributes
through the `number` arm too, so it checked a matrix's first gain and no others. Nobody noticed
because the reader checks every element on load. It has its own list branch now.

**Four places where the text above was short, corrected here rather than left to be found:**

- **The door refuses what `validate()` refuses, and not only `validate()`.** The table says
  `validate()` refuses a bad curve, and it does. But `validate()` refuses the *file*: a curve the
  door had applied would save, and the next open would refuse the show, so one datagram would
  leave a show that does not open. The door answers `bad-value` for a list whose elements are
  numbers but which is not a curve. For the same reason it answers `bad-value` for `gains` that
  are not whole rows as wide as their bus or slot. That rule, `coefficientsFit`, is now the one
  function `validate()` asks too. Before this PR no client could write a list at all, so the door
  had never had the question to answer.
- **A drawn fade leaves from where the run is, not from its first breakpoint.** This section
  called the levels absolute, which is right for every breakpoint but the first. The first is
  where the drawing starts on the page. The fade starts wherever the level has got to, which is
  the rule `FadeJob.h` already keeps for a fade taking over from a fade: anything else is a jump,
  and a jump on a PA is a click. So the first segment runs from the run's level to the second
  breakpoint, and every breakpoint after the first is met exactly, at its time.
  *Proposed, and the author's to overrule.* The alternative, honouring the drawn start and
  jumping to it, is one line in `cue::fadeLevelDb`. 5.16b's editor should show the first point's
  level as the level the fade leaves from, not one that can be dragged.
- **`level` is not read either, as well as `curve`.** Absolute levels make the last breakpoint
  where the fade ends. `level` would have been a second answer to that question, so where points
  exist it is ignored, and the CSV descriptions of both words say so. 5.16b's inspector should
  grey them out.
- **`route.create` had never survived a save.** It makes a Route with no gains, and the writer
  omits an empty list as the default it is. The reader, though, requires every `persist=show`
  row with no default unless `""` is a value of its type, and it classed a list with the
  numbers. So the reopen refused the file: *"`<Route>` must carry `gains`"*. `createFeed` had the
  same hole. For a list, as for a string, `""` is a value (the empty one), so the rule now skips
  lists as it skips strings (`CanonicalXml.cpp`). Absent gains are the empty matrix, which
  routes nothing, exactly as `gains=""` always did, so neither row gains an identity default
  (§3.9b). Nothing had tripped over it: every test that saves a Route gives it gains by hand
  first, and no client could give it any. `MediaCueTests` now pins the round trip. `points` was
  what found it, because an empty curve is the ordinary case.

**Two hazards found and not fixed, for the author.**

- **A re-pointed destination can leave a show that does not open.** Re-pointing a Route's
  `bus`, or a Feed's `slot`, at a destination of a different width is applied by the door. The
  next open then refuses the file for gains that no longer divide, and that has been true since
  Phase 2. The remedy is the same `coefficientsFit` asked when a `bus` or `slot` is written. It is
  not in this PR because it changes what a scalar write to a Phase 2 row means, and that deserves
  its own sentence in §13.
- **A list sent as N arguments is written as its first.** A datagram to a node becomes `node.set`
  with `packet.args.front()` and nothing after it (`oscquery/EngineNamespace.cpp`). So a generic
  OSCQuery client that reads `TYPE "dddd"` and sends four doubles writes one. For a curve, or for
  gains into a bus two or more wide, the result is refused as `bad-value`, which is at least
  honest. For gains into a bus one channel wide it is applied, and it is the wrong matrix. The
  console sends a list as one string, as §14.2's *one value, as text* says a client should, and
  5.16b's editor will too. Joining N numeric arguments into the list's text, for document list
  rows only, is a few lines in the namespace and the `node.set` handler. It is also a decision
  about the wire contract, and mounted namespaces take the same first-argument road, so it is the
  author's.

**The console reads a list as one text field** (`views/inspector.js`). It holds the whole list,
space-separated, and is written back as one `node.set`. Read as a scalar before, a list field
showed only its first element. An empty list was not shown at all, because the tree serves it
with no `TYPE`: there is no tag to give zero values. This is a reading, not the editor: 5.16b's
curve is the view.

**Tests.** `DocumentTests`: a list written and read back whole, and the refusals. A curve's ten
ways of not being one. A file holding a bad curve refused at load, naming it. `GoTests`, where
the fade cases live (the plan said `RunTests`): the arithmetic on its own; a two-second fade
that dips to −30 lands on it exactly at its time and on −10 fifty ticks later, with `level` at
−120 and `curve` at `sCurve` both ignored; a run at −6 leaves from −6 and not from the drawn 0.
`UndoTests`: the list row that the `toVar` agreement case was waiting for (*"the row which
arrives first is added to this case"*), and a redrawn curve undone to the one before.
`MediaCueTests`: a route with no gains survives a save and a reopen. And the replay fixture
`fade-curve.wfglog`, with its bundle. It holds a curve drawn during the session by one
`node.set`, and two drawings refused with the reason each deserves. Run against the old door, it
fails on the first `node.set`; that was tried. As `fade-stop` says of every fade, it pins the
session and not the levels, which the unit suite checks sample by sample.

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

**And a seventh readout, added 2026-09-17 for the compiled client's first milestone (M0 of its
plan):** `/godot/document/revision` publishes `ShowDocument::showRevision()` from the same after-tick
line that derives `dirty` from it. What moves it is what `show.xml` would record - an edit, an undo,
a revert, a recovery, a load; what leaves it alone is the point: a GO, a standby move, a focus change
and every run, because none of them is a change to the show. The desktop's cue list keys its cached
rows on it, and needed a key because the snapshot's own document half is the wrong one - `markStale`
at `Console.cpp:2987` fires on ANY applied command, so a chain of runs mints a fresh document half
several times a second for a show nobody edited. Not `revision()`: that counter (`ShowDocument.h:466`)
bumps on state rows too, so an arrow key would have rebuilt the list. It reads from 1 and never 0, so
a client can keep 0 for "no picture built yet". The page shows it on the tech line beside the tick,
which is §14.13's rule and nothing more - the page has no cache to key.

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
Event&, const std::vector<osc::Value>& args, std::int64_t tick)>)` beside `setLogging` —
vendor-free, as `Engine.h:36-39` requires, `osc::Value` being Go.dot's own and already in that
surface through `CommandRegistry.h` — mirroring `TickThread::setBeforeTick`
(`TickThread.h:162-168`), and serve and replay both install
`document.beginTransaction (command.name, tick, origin, args)` (§14.13). *The fourth parameter is
a correction (PR 5.4, 2026-09-10):* this paragraph first drew a three-argument hook and then
required it to fire on `check.args`, and those two sentences cannot both hold — `Event::args` is
the SUBMITTED list held by value, so a hook handed only the event can never see the coerced
arguments. The alternatives were a synthesised `Event` carrying the coerced list, or mutating the
caller's, and both cost a deep copy per event on the tick thread to hand the Console an event
that is not the one that arrived. Where it fires
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
makes the reproduction a ctest. *Two corrections (2026-09-16):* the fixture was named by this
plan and by §14.9 and **was not written by PR 5.4** — an audit of what Phase 5 still owed found
it missing while both sections spoke of it in the present tense, and it landed today, twenty-two
records against `tests/fixtures/bundles/minimal`, registered as `wfg.replay.undo.C` and
`.fr_FR`. And the check is `wfg replay`'s exit code, not `diff -r` against a saved bundle: no
fixture in the tree has ever diffed a directory, because a replay that reproduced every record
and left a different document would be a divergence the records themselves did not show, which
is not a thing the log format permits — the document is rebuilt BY the records. The seam no
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

*Corrected 2026-09-17.* One tick was too short for one holder of the far side: a virus scanner. On
a shared Windows runner (`be46383`'s run) the Phase 1 driver's save never landed - `ReplaceFile`
failed, the one retry after 20 ms failed, the write was refused as designed, and the dirty dot
stayed lit for the fifteen seconds the driver waited. A booth PC with its scanner on is the same
machine. The author's rule, put to them that day: *"Save can have some lag so it doesn't interfere
with the live show"* - and since PR 5.5's second half the pause is on the writer thread, where lag
delays the next write and nothing an operator can feel. So the retry is now three pauses - 20, 60
and 200 ms, a third of a second in all - before the same honest refusal; a failure that survives
that long is still not a transient one. Not a loop, for the reason above. The same conversation
reaffirmed that the lock does not touch the save (§9, decision W); what changes is that show mode
on either client does not OFFER one.

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
redesign. *Corrected 2026-09-11, at the author's direction:* M23 came back at four times the
threshold (§14.14), the fallback was taken, and the answer to the constraint is no longer a
single late tick but no disk at all: the tick thread takes the snapshot — 1.4 ms on the 500-cue
show — and a writer thread writes the bytes, so what a GO can wait behind during an autosave is a
serialisation and a queue push. The second half's paragraphs below say how.

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
available`, publish `/godot/document/recovery = true`, and do nothing else. *(Corrected
2026-09-11, at the author's direction: finding none, it looks for the highest-numbered
`recovery.previous.N/`, an afternoon an earlier session moved aside unanswered, and offers that
the same way — the second half's paragraphs below.)* Adopting it silently
would be wrong three times over. It would make the show on screen differ from the file the
operator opened with no gesture in between. It would decide on their behalf that the abandoned
afternoon was worth keeping, which is the one decision autosave exists to leave open. And
`adopt` replaces the root wholesale (`ShowDocument.cpp:315-332`), lock included, so a silent
adopt would also change `Show/@locked` — a show that came back unlocked because a file on disk
said so, during a performance, is §14.11's nightmare arriving through this subsection's door.

| command / flag | what it does | notes |
|---|---|---|
| `document.recover` | adopts `recovery/show.xml` and `recovery/state.xml`, clears the undo history, `markStale` — *since 2026-09-11, from wherever the offer now lives, `recovery/` or a `recovery.previous.N/`, after draining the writer* | leaves the document **dirty**, deliberately: the recovered work is not on disk as the show, and the dot is telling the truth. `reason::noRecovery` when there is nothing to adopt — *and since 2026-09-11 when nothing is offered, whatever this session's own `recovery/` holds* |
| `document.discardRecovery` | deletes the folder — *since 2026-09-11, the offered one, wherever it lives, as a job on the writer* | `no-recovery` likewise; refusing an empty gesture is cheaper than pretending it worked |
| `wfg serve --recover` | applies the recovery before the first publish | for scripts and for the black-box driver, which has no person to click. It goes in the **usage string** as well as the parser (`Console.cpp:2421-2423`): `--device` and `--device-type` are parsed at `:1891-1923` and appear in no usage line, an omission already one phase old, and the usage string is what an operator reads at 04:12 |

A successful `document.save` deletes `recovery/`, because the work has become the show. A clean
exit deletes it **only when the document is not dirty** (`Console.cpp:2315-2329`, after
`ticks.stop()` has joined the only writer) — a tidy shutdown with unsaved work is the case the
folder exists for. *(Corrected 2026-09-11: both delete only this session's own `recovery/` — never
while it still holds an earlier session's unanswered offer, and never a `recovery.previous.N/` — and
the clean exit drains the writer first, so a save queued at Ctrl-C lands before it asks whether
the document is dirty.)* And of the four verbs that open a bundle through `Bundle::open` — serve at
`Console.cpp:1440`, `wfg tree` at `:871`, `wfg validate` at `:1010`, `wfg replay --bundle` at
`:509` — only `validate` will say the word: one line noting that `recovery/` is present and was
not validated, because `validate` is what somebody runs on a bundle they suspect. A tree dump
and a replay are not asking about unfinished work. *(Since 2026-09-11 it prints a line in the same
voice for every `recovery.previous.N/` beside it, lowest N first.)*

**Three things building it found that this subsection did not draw** *(PR 5.5, 2026-09-11)*.

- **The first autosave would have destroyed somebody else's afternoon.** The paragraphs above
  cover a recovery the session itself wrote. They say nothing about one found at open and not yet
  answered — and as drawn, this session's first autosave, two seconds after its first edit,
  writes `recovery/show.xml` straight over it: the abandoned afternoon gone before anybody read
  the banner offering it back, which is the one decision this subsection exists to leave to the
  operator. The first build closed it the conservative way — while a recovery found at open was
  unanswered, nothing deleted it and **autosave was suspended** — which traded the new work's
  crash protection for the old work's safety. *Decided 2026-09-11, at the author's direction:*
  neither is traded. The unanswered recovery is **moved aside** to a `recovery.previous.N/` before
  this session's first autosave lands, and the autosave keeps running; the second half's
  paragraphs below give the rule.
- **`revert` and `recover` read into a scratch document, then adopt.** `CanonicalXml::read`
  refuses what it cannot parse before touching anything, but its last pass, `validate()`, runs
  *after* `adopt` — so a revert of a show that failed a whole-document check would come back
  refused having already replaced the show and cleared its history, with a log line saying nothing
  happened. The scratch document is handed over only when every step succeeded.
- **`saveAs` copies `namespaces/` first and writes the manifest last.** Copied after `save`, the
  descriptions arrived in a folder that was already an openable bundle, so a crash between the two
  left a bundle whose mounts described nothing. Now the manifest's arrival means everything else
  has, and a crash part-way leaves a folder `open` refuses rather than half-loads. The black-box
  driver found it, reading a description at nought bytes.

**The second half, as the author decided it on 2026-09-11** *(PR 5.5)*. Three decisions, final,
and what building them found.

**The bytes leave the tick thread — a save's and a copy's with the autosave's.** M23 answered at
four times the threshold (§14.14), and the switch this subsection drew is thrown. The tick thread
takes the snapshot — `Bundle::snapshotOf`: both serialisations and the `showRevision()` they are,
1.4 ms on the 500-cue show, which it must take because the document's single writer is what makes
reading it safe — and a `DocumentWriter`, one thread on `MountProbe`'s shape, writes the bytes.
`document.save` follows the autosave there, at the author's direction: a person's gesture rather
than the engine's, but the same twenty milliseconds on the thread GO shares, and a Ctrl-S during a
performance would cost the GO path exactly what an autosave did. `document.saveAs` goes on the same
writer for the same reason. A writer thread silently breaks four things, and each is answered by
construction rather than by care:

| what a writer breaks | the answer |
|---|---|
| **ordering** | one writer, FIFO. A save and an autosave queued in that order land in that order; a save's deletion of this session's `recovery/` happens on the writer, after the save's own bytes and before anything queued behind it — queued on the tick thread instead, an autosave submitted after the save could land first and then be deleted by it |
| **the stamp's revision** | the revision travels with the snapshot, to the writer and back. `savedRevision` and `autosavedRevision` are stamped from the revision the snapshot was taken at, when a completion says the bytes landed — never from `showRevision()` at confirmation, which would mark an edit made while the write was in flight as saved. Completions come back through a queue the after-tick drains (`settle`), so the tick thread stays the only writer of `DocumentSession` and of the show |
| **a read after a queued write** | `document.revert` and `document.recover` read files, so they **drain** the writer first — the one place the tick thread waits for it. The wait is bounded by what is queued, a handful of write-flush-replace jobs; it is acceptable for these two because they are a person's deliberate gestures that replace the whole show, and the lock refuses both during a performance before the wait is reached. GO reads no file and never waits |
| **what a failure looks like** | the command is applied — its `A` now means *taken, and handed to the writer* — and a failure comes back through the completion: `savedRevision` stays unstamped, so the dot stays lit, which is the truthful signal; and the writer's own sentence, naming the file, is published at **`/godot/document/writeError`**. Not at `lastError`, which quotes a refused record by tick and sequence: this failure has no record, and folding it into `errorCount` would make the count disagree with the log. It goes out when a later write of the same command lands, or when a save lands over an autosave's failure |

The checks that need no write stay on the tick thread and still refuse at once: a `document.saveAs`
with no path, and an autosave into a bundle that has gone — one `stat`, and the refusal an
unattended writer most needs to leave in the log. **Replay writes inline**: `wfg replay` hands the
commands a synchronous writer that is never started, because a replay has no GO path to protect
and a thread there would only add nondeterminism; the record is identical either way, as §14.14
promised. **Shutdown drains the writer** before the clean-exit tidy-up asks whether the document
is dirty (`finishSession`), so a save queued at Ctrl-C lands; a kill loses what was queued, and the
atomic write leaves every file old and whole or new and whole.

**An earlier session's unanswered recovery is moved aside, and the autosave keeps running.** The
rule, whole:

- `recovery/` is always this session's own autosave target.
- The recovery a session **offers** — what `/godot/document/recovery` and the banner are about — is
  `recovery/` if `recovery/show.xml` exists, because then the last session died; otherwise the
  highest-numbered `recovery.previous.N/`, if there is one.
- The first time this session needs to autosave while the offer still sits in `recovery/`, the
  writer **renames** it to the next `recovery.previous.N/` — in queue order, before the autosave's
  bytes — remembers where the offer now lives, and autosaves as usual. N is a counter, never a
  clock: one past the highest in use.
- `document.recover` and `document.discardRecovery` act on the offer wherever it now lives, and
  answering clears it for this session.
- A save deletes only this session's own `recovery/`; a clean exit deletes only this session's
  `recovery/`, and only when the document is not dirty. **Nothing deletes an unanswered
  `recovery.previous.N/` but its own discard**, so an afternoon nobody answered is offered again at
  the next open, newest first.
- `wfg validate` names every `recovery.previous.N/` present, beside the `recovery/` line.

*Refined 2026-09-11, at the author's direction, once `recover` is also an answer:* a recovery
adopted from a `recovery.previous.N/` is **consumed** — not deleted by the recovery, because a
crash straight after it must still lose nothing and the recovered show then exists only in memory
and in that folder, but deleted on the writer, in queue order, by the first autosave or save that
lands after it and so puts the recovered work on the disk under a name this session owns. Without
the refinement, an afternoon recovered and then saved would be offered again at every start until
somebody discarded work that was already the show. Adopting from `recovery/` itself consumes
nothing: that folder simply becomes this session's own. And because `recovery/` then holds this
session's older autosave rather than the recovered show, `autosavedRevision` is zeroed, so the
catch-up autosave comes at the next quiet. `wfg serve --recover` adopts through the same function
as `document.recover`, so the two cannot come to mean different things.

**A session that recovered does not replay, and says so.** The recovered bytes are covered by
neither the log nor the bundle its header hashes, so a replay past the adoption builds a different
show and reports the difference as divergence — sending somebody hunting for non-determinism in an
engine that has none. Two ways a session adopts, and both are caught (`Bundle::replayBoundary`): an
**applied `document.recover` record** stops the replay there, with the records before it still
replayed and held to reproducing exactly; and a session started with **`wfg serve --recover`**,
which adopts before its first record and so leaves nothing to point at, carries a header line —
`# recovered recovery.previous.2/`, the folder relative to the bundle — and is refused up front. The
exit code is **2**, the replay's *could not run*, and not 1: the three codes exist so that "the
inputs are not enough" and "the engine is not deterministic" are never mistaken for each other, and
this is the first. A prefix that diverges before the recovery is still exit 1, with the refusal
printed after the mismatches.

**What building the second half found** *(2026-09-11)*.

- **`document.discardRecovery` is a job, not a wait.** Drained like `recover`, it would have been
  the one command the lock lets through during a performance that could hold a GO behind a disk.
  Queued on the writer instead, it costs the tick thread a push and runs behind any rename already
  queued — so it deletes the offer where the rename put it, not where it used to be. The offer is
  withdrawn by the completion that says the folder has gone, a tick or two after the gesture.
- **An offer whose folder somebody deleted by hand is withdrawn**, by the next autosave that goes
  to move it or by the next answer. Kept standing, it would have failed every autosave for the rest
  of the show on a rename of nothing, and left a banner nobody could dismiss.
- **"The first free N" is read as one past the highest.** Every gap the engine makes is at the top
  — the offer is always the highest, and a discard deletes the offer — so the two readings agree on
  every folder the engine leaves, and differ only after somebody deletes a middle one by hand, where
  the lowest gap would file the newest afternoon under the oldest number.
- **Known, not built: a session that discarded an earlier session's recovery does not replay.** The
  replay's `--out` never holds that offer, so the replayed discard is refused `no-recovery` where
  the live one applied. It predates this half — the first half's discard asked the disk for a folder
  `--out` never had — and the fix is a design choice rather than a repair: a header line marking the
  offer found at open, and a placeholder standing in for it in `--out`.

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

**THE LOCK'S FIRST END-TO-END CONFIRMATION, and how it nearly became a bug report (2026-09-17).** A
`wfg serve --window` of a freshly copied, unlocked `phase4` published `/godot/document/locked` as
true. `wfg tree` over the same folder said false, so did a serve without `--window`, so did four
later runs of the same command - and the one run that was logging recorded no write at all. It was
written up here as unexplained and a check was added to `phase1_session.py` against a race nobody
had found. **The author then said what it was: they had pressed the window's lock button.** The
engine was right, the window was right, and the node said exactly what somebody had just told it to
say.

It is kept here because of what it cost to not know. **Every reading in that paragraph was
consistent with a human hand and I never considered one**, because the session was a smoke test and
I was thinking of the window as a thing I had started rather than a thing on somebody's screen. The
one run that could have answered it in a second was the one I gave `--log` to - the wrong one - and
the session that showed the reading wrote no record, so the origin that would have read `window`
was never written down. **The rule that follows: a `--window` session is an INTERACTIVE session and
always gets `--log`.** A window is reachable by whoever is sitting there, the log is what tells a
click from a datagram (§14.16, rule 1), and a measurement taken without one cannot say which it
was.

The check in `phase1_session.py` stays, on its own merits rather than as a net: a show whose
`state.xml` names no lock must open unlocked, the `locked` fixture asserts the other direction
already, and between them the pair pins both readings of a `persist=state` boolean at open. What it
is no longer is evidence of anything.

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

*Overruled by the author, 2026-09-25, with the D700's LEDs and the waveforms in front of
him.* **The stops lean blue**: *"I would bias a bit towards the blues"* - deep blue at 250 Hz,
red (through violet) at 800 Hz, orange at 2.5 kHz, yellow at 6 kHz; purple at 40 Hz and green
at 12 kHz as they were. The body of most material had come out red, orange and yellow. **And
saturation is noisiness where the energy is**, no longer `1 − flatness` over the whole band,
which read only white noise as grey: each bin against its own neighbourhood, a ninth of an
octave either side, weighted by the neighbourhood's power - grey for noise of any colour or
bandwidth, vivid for a tone (*"don't desaturate on a broader, noisier signal which I find
quite a telling visual cue"*) - **and below 200 Hz sound reads by its pitch**, never as noise:
a 2048-sample window cannot tell a tone from noise there, and a sub-bass pulse the author
compared with Samplitude's dark blue had come out grey (*"It's like pulsating bass drum"*). A
rumble reads coloured too; that is the price. The analysis's format version is 4.

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

**What PR 5.6 built (2026-09-14), and one thing its review changed.** The durations are a
`const` member, so the compiler refuses any later write, and `MediaInfo` can be neither copied nor
moved, so the address cannot drift; `MediaCueTests` pins the law where it bites — publish,
publish, publish a record, publish, and the slot analysis rebuilds nothing, while the same numbers
at another address rebuild exactly once, which proves the count can move. The first build refused
to publish a path the show had not named at open, to keep the two halves' keys identical. *That
was corrected in review:* the plan queues 5.7's analyser for any file a `media/file` edit
introduces mid-session, and a refusal would have left an imported file grey until the show was
reopened. So `publish` takes any path; a path both halves know has its seconds forced to the
frozen value, and one the show did not name at open keeps the seconds its publisher read. The
snapshot may therefore name a file `durations()` does not — never the reverse — and `durations()`
never grows, which is all the law ever required.

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
level table, little-endian. *(Both refined by PR 5.7, 2026-09-14, and said at the end of this
section: a silent frame lends a coarser one nothing, the hue is averaged by saturation, and the
file carries a checksum and the sample count it describes.)* The reason is one sentence of
§3.30's and it is the whole design:
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
property a machine can check and an eye cannot. *Corrected by PR 5.7 (2026-09-14): not with
these stops.* Purple at 40 Hz is 280° and deep blue at 150 Hz is 240°, and from there the ramp
climbs the other way round the wheel through red, orange and yellow to green — so between 100 and
150 Hz the sweep's hue falls before it rises, and the test as written would have failed a
correct build. What is monotonic by construction is the lightness. So the sweep is asserted twice
over: its lightness never falls, and every frame's hue is the ramp's hue *at the sweep's
frequency at that instant*, which is a formula — a stronger check than monotonic would have been,
and still one a machine makes. Three signals whose answers are known in
advance, which is the difference between a test and a screenshot. It is written twice,
deliberately, and the second is the one that counts:

| where | what it asserts |
|---|---|
| `tests/TimbreTests.cpp` (new) | the sine is saturated above 0.8 at the ramp's 1 kHz hue within tolerance, at 44.1, 48 and 96 kHz; noise is under 0.2; the sweep's lightness never falls and each frame's hue is the ramp's at the sweep's frequency then (*not* "monotonic", PR 5.7's correction above); level *k* is level *k−1* paired by the rule restated in the test; write/read round-trips byte-identical and every damaged file is refused; a second `analyse` reports zero work |
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

**What PR 5.7 built (2026-09-14), and five things building it changed.** `audio/Timbre.{h,cpp}`
is the arithmetic and nothing else — samples in, a pyramid out, the pyramid to bytes and back —
and `audio/MediaAnalyser.{h,cpp}` is the file, the hash, the cache and the thread. `wfg analyse`
and `wfg serve`'s analyser call one function, `analyseMediaFile`, so what the verb prints is what
a session pays. A frame describes one hop's stretch of the file, with its 2048-sample window
*centred* on that stretch, so the colour drawn over a stretch is that stretch's; the channels are
averaged for the spectrum and the peak is taken over all of them, so a stereo pair in opposite
phase still has a waveform. The three signals came out where the arithmetic said they would, at
48 kHz: the sine saturated to the byte in every steady frame, its hue within the byte's own
precision of the ramp's 18.9°; the noise at a mean saturation of **0.153** — 0.176 in its
least grey frame, so under 0.2 frame by frame and not only on average — and a lightness of
0.73, bright, because white noise's power is where the bins are; the sweep within **0.9°** of the
ramp at its instantaneous frequency and 0.0025 of its lightness, at worst. The five changes:

- **The sweep's check**, corrected in place above: the ramp's hue turns back between purple and
  deep blue, so what is asserted is the lightness, which never falls, and the hue against the
  ramp at the sweep's frequency frame by frame. The stops are the author's; a monotonic hue
  would need the 40 Hz stop on the blue side of 240°.
- **Silence has no colour, and lends a coarser level none.** A frame whose in-band power is below
  what a −100 dBFS sine would put there has hue, saturation and lightness all nought — lightness
  nought is below the ramp's darkest, 0.15, so it cannot be read as the bottom of the ramp. The
  circular mean this section drew would have averaged it in: silence would have halved the
  lightness of a frame half-silent — a lie in the one dimension that is the frequency axis — and
  its hue, nought by convention, is *red*, so every quiet stretch would have tinted red at the
  coarse levels a Gogo bar reads. So a silent frame paired with a sounding one gives the sounding
  one's colour, and between two sounding frames the hue moves along the shorter arc by the second
  frame's share of the two saturations — a grey frame has no hue worth averaging, and a vivid one
  dragged halfway to it would be a colour neither had. Integer arithmetic throughout, a half
  rounded up, written out in `Timbre.h` because the test and the driver each restate it.
- **The flatness is on magnitude, and now it is measured rather than argued.** On power, the
  geometric-over-arithmetic mean of exponentially distributed bin powers is *e* to the minus
  Euler's constant, 0.56, whatever the level — noise would read a saturation of 0.44 and could
  never be grey. On magnitude it is about 0.85, and the noise read 0.153.
- **The file carries a checksum and the samples it describes**, because it is written *without*
  being made durable: a show is flushed to the disk before its name moves, a pyramid is arithmetic
  anybody can repeat, and on the Windows box M23 found two durable replaces cost nineteen
  milliseconds, nearly all of it flushing and replacing — an import of two hundred files has no
  show's sake to pay a share of that for. The temp and the replace stay, so no reader ever opens a
  half-written file. A power cut can leave a file of the right length and the wrong
  bytes; `timbre::read` checks the FNV-1a of everything after the header, every level's size
  against the one below it and the level count against the halving rule, and refuses anything
  else, and the analyser builds it again. `formatVersion` is bumped by any change to the
  analysis, a moved stop included, because the key is the content alone and the version is the
  only thing that tells a pyramid computed by an old rule from one computed by this.
- **`juce::SHA256 (const File&)` is the wrong one of the two spellings.** It hands the digest a
  bare `FileInputStream`, and JUCE's SHA-256 reads its stream 64 bytes at a time — a read from
  the operating system each, sixteen million for a gigabyte. The stream spelling is used instead,
  behind a 64 KB buffer and behind a stream that answers "no more" the moment the analyser is told
  to stop, so a Ctrl-C does not sit through a gigabyte's hash; a digest of a prefix is thrown
  away by a caller that looks at the flag before it looks at the hash.

The rest is as drawn. The analyser is queued in the show's order, so cue 1's sound has its
colours before cue 90's; a path is queued once a session, so the tick thread can re-offer every
file the show names after any *show* edit — a GO moves standby, a state row, and walks nothing —
and all but a newly imported file are dropped under one short lock each. A record is published
only with its pyramid, so none carries a hash the §14.5 route would refuse. `wfg analyse` prints a
line per file, the path last because it is the one field that may hold a space, formats every
number without the locale, and exits 0 when every file the show names has a cache on disk
afterwards, 1 when one does not, 2 when there was no bundle to open. And one thing found while
copying `MountProbe`'s shape: its `stop()` raised the flag *outside* the lock the thread tests it
under, so a stop landing between the thread's test and its sleep was a notify nobody heard and a
join that never returned — a Ctrl-C that hangs, once in a long while. Both now raise it under the
lock.

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
  author has to have right. A lock that is not set — never set, or set and then released — is
  **absent** from `state.xml`, because `CanonicalXml::attributeText` omits an attribute in BOTH
  cases: when the property is missing (`CanonicalXml.cpp:80-81`) and when its canonical text
  equals the row's default (`:141-142`), which for `locked` is `false`. So the first
  `node.set /godot/document/locked false` after a lock leaves no `<Show>` entry at all, and a
  fixture asserts the absence. *This sentence is a correction (PR 5.3, 2026-09-10):* it said the
  opposite, that the writer omits only a missing property and so a released lock is written as
  `locked="false"`. A verifier read the `hasProperty` test at `:80-81`, stopped there, and missed
  the default comparison sixty lines further down the same function; PR 5.3's own unit test,
  written to that account, is what caught it. What still round-trips is a `false` somebody *wrote
  by hand*: `<Show locked="false"/>` in a `state.xml` reads back into an unlocked document. And
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
  format this build's reader lacks and the pair if it does not. *(PR 5.7, 2026-09-14: the
  pair, `blackbox.timbre.C` and `.fr_FR` — the driver writes WAV, which every build reads, and
  the verb prints numbers a comma would break.)*

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

**M22 answered, on the Windows box (PR 5.7, 2026-09-14): the analyser stays a thread nobody
mentions.** A Release build, five minutes of stereo 24-bit audio at 48 kHz — 86.4 MB of WAV —
analysed three times with `wfg analyse --force`:

| part | five minutes of audio | per minute of audio |
|---|---|---|
| the analysis: decode, 14 063 frames coloured, nine levels built, the cache written | 367–391 ms | **0.073–0.078 s** |
| the hash, SHA-256 over the file's bytes | 344–356 ms | 0.07 s — about 245 MB/s |
| the cache on disk, every level | 112 396 bytes | **22.5 kB** |

About eight hundred times faster than real time, with the hash costing as much again: an hour of
material is under ten seconds of background work at an open, on a thread GO does not share, and
the operator is told nothing — §3.30's claim, answered rather than obeyed. The bytes are the
estimate above, doubled by the levels as a pyramid doubles anything: 11 kB a minute at the finest
level and as much again in the eight above it, about 1.35 MB an hour, so `media/.timbre/` stays
where it is. What the figure also says is where the time would go at scale: a file already cached
still pays its hash at every open, because the hash is the key — two hours of stereo 24-bit is
about eight seconds of hashing per session, off the GO path. A memo of path, size and time
against hash would remove it, and nothing here needs it yet. **The Mac mini's figure is owed**,
beside M23's.

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

**M23 answered, on the Windows box (PR 5.5, 2026-09-11): the bytes leave the tick thread.** A
Release build, the 500-cue show at 65 616 bytes of `show.xml`, a hundred autosaves, twice:

| part | median | 99th percentile | worst |
|---|---|---|---|
| `CanonicalXml::write` alone | 1.37 ms | — | — |
| the `document.autosave` handler: both files written, flushed and replaced | 20.8–21.3 ms | 24.7–26.4 ms | 26.6 ms |
| the threshold, a quarter tick | 5 ms | | |

Four times the threshold and a full tick at the median, and the serialisation is not why: 1.4 ms of
it is Go.dot and the other nineteen are the durability — `FlushFileBuffers` and `ReplaceFile`,
twice. The size does not explain it either: a stand-in run of the raw write on this box jumped
between 2 KB and 16 KB, which fits real-time antivirus scanning each replaced file, and Defender is
on. So the answer is the platform's, which is the one kind of answer a faster Go.dot cannot move,
and it is the answer this subsection drew the switch for: **5.5's second half takes the snapshot
on the tick thread — the 1.4 ms — and hands the bytes to a writer thread on `MountProbe`'s
shape.** The record is `document.autosave` either way, so no fixture and no assertion changes.

**And re-taken once the switch was thrown (PR 5.5's second half, 2026-09-11)**, the same show and
build, the instrument now timing the two threads apart:

| | median | 99th percentile | worst |
|---|---|---|---|
| the tick thread: snapshot, the folder's stat, the handoff | 1.79 ms | 2.30–2.44 ms | 2.48 ms |
| the writer: both files written, flushed and replaced | 19.1–19.5 ms | 23.1–24.7 ms | 24.9 ms |

The GO thread's share went from 21 ms to under 2, and its worst case sits at half the threshold;
the nineteen milliseconds are still spent, on a thread GO does not share. The
same measurement says every `document.save` has cost more than a tick on this box since PR 5.2 made
it durable, and the author decided (2026-09-11) that it follows the autosave onto the writer, with
`document.saveAs` beside it: a habitual Ctrl-S between cues is still a disk on the GO thread,
whoever chose the moment. The price is that a save's failure arrives a tick or two later, at
`/godot/document/writeError`, rather than as a refusal — §14.10 says why it is not `lastError`.
The Mac mini's figure is still owed; the macOS CI runner's, in a **Debug** build, is a 13 ms
handler of which 11 ms is serialising, so that platform pays about two milliseconds for the
durability this box pays nineteen for — which is what an antivirus explanation predicts and no
Go.dot explanation would.

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
moving for a reason other than the author being satisfied with it. *(Wording corrected by PR 5.9:
the two things to measure apart are the two render costs 5.9 changes — the rows, §14.3's first
defect, and the trigger scan, §14.3's index. §14.3's second defect, the slider's guard, costs no
render time at all.)*

**M24 answered, on the Windows box (PR 5.9, 2026-09-15): the page can hold the show.** Headless
Edge 153 against a **Release** engine serving M18's shape — five hundred media cues over twenty
slots, plus seventeen groups and twenty triggers, 517 rows, written by
`tests/fixtures/make_large_show.py` — and timed by `scripts/measure-console-render.py`. The
instrument wraps the page's own functions by name, is installed before the page's first request,
and lets ten polls go by. The page before 5.9 (`--ui-rev 7154bb2`) and the page after were run
back to back, three times over:

| per poll, median (worst) | the page before | the page after |
|---|---|---|
| `render()` | 559 (577) · 539 (570) · 568 (575) ms | 6.8 (7.5) · 6.3 (6.8) · 6.8 (10.9) ms |
| the trigger scan, 517 calls a render | 541 · 524 · 548 ms | 1.2 · 1.1 · 1.2 ms |
| the rows (the third pair: less the overlap scan's 0.6 ms, which neither version changed) | 18.6 · 16.1 · 17.4 ms | 5.0 · 4.7 · 4.3 ms |
| the layout a render leaves behind | 19.8 · 18.5 ms | 6.9 · 6.5 ms |
| polls apart | 594 · 599 ms | 115 · 110 ms |

**The scan was 97 % of the page's time**, and it was never the rows: keyed rows took the rows
from about 17 ms to about 4.5 and the layout after them from 19 to 7, and the index took the scan
from half a second to one. Before, the page spent more than half of every second inside `render()`
and drew at under two polls a second, with the main thread too busy to take a click. After, a
render is about a fifteenth of the poll. **Decision T's premise holds on this machine: the page is
the operator client, and neither a windowed strip nor an early `ListBox` is called for by this
number.** The same runs also give the three behaviours a before-and-after. Space from a
keyboard-focused slider reaches GO after and did not before, and the arrow moves the standby
rather than the slider. Escape restores a field and writes nothing. The list keeps its scroll in
both, which is §14.3's correction.

**Two costs M24 was not asked about, recorded beside it because the page's own comments point
here.** *The tree is 8.6 MB* at 500 cues. The Release engine serves it in about 60 ms and the
reply takes about 30 ms to cross loopback, while the Debug engine takes about a second. *And the
page used to ask ten times a second whether or not it had been answered*: against a Debug engine
that queued about 1350 requests in the browser. Past the browser's cap every new one then failed
at once, and in four measured runs of seven the page drew once in four minutes, or never. One poll
in flight fixes the second of these, and after it the Debug engine's page draws in 1.3 s at a
poll a second. The first is the next wall, and it is not the page's. Ten polls a second of 8.6 MB
is 86 MB/s, which loopback carries and a tablet's Wi-Fi would not. So the show a tablet can follow
at ten a second is bounded by the transport, long before `render()` is the limit. LISTEN pushes,
which the server already speaks (§14.2), or a poll that asks for values rather than the whole
description, are the two ways past it. Both belong with 5.10's `plumbing/`, and choosing between
them is the author's call. That figure is inferred from these sizes and not measured on a tablet,
which is owed. **The Mac mini's M24 is owed too**, beside M22's and M23's.

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

**M25 — WHAT AN OPEN WINDOW COSTS THE THREAD THE SHOW RUNS ON (taken 2026-09-18, and it took three
takes to be worth reading).** Release build, 500-cue generated show, hosted clock, 120 s per
condition, `/godot/engine/lateness` sampled at 10 Hz. Conditions C and D want a running pane and a
scrolled list and arrive with M4; this is A against B.

| | A no window | B window, idle |
|---|---|---|
| median lateness | 0 samples | 64 samples (1.33 ms) |
| p95 | 64 samples | 64 samples |
| worst sample | 192 samples | 192 samples |
| `latenessMax` | 832 (17.3 ms) | 768 (16.0 ms) |
| samples over one tick | 0 of 1200 | 0 of 1200 |
| `rtViolations` | 0 | 0 |

**GREEN on the plan's own threshold** - B within one tick of A - with room to spare: the only
difference is 1.33 ms of median lateness, seven hundredths of a tick, and the worst case is
marginally *better* with the window open, which is noise. An idle window costs the tick thread
nothing measurable. **What is NOT measured here:** `go → sound`, which wants `first_sound.py` in A
and C, and the client's own repaint times, which want an instrumented build. A take on a real device
is the one to quote; this is a hosted clock, because the device is the author's to choose and a take
at night should not make a sound.

**AND THE BUSY CASE, which is the one a show cares about (same take, `--firing`: a GO every two
seconds).** It could not be taken until M3 gave the window a list to draw, and it is half of the
plan's condition C - nobody is scrolling.

| | A' no window, firing | C window, firing |
|---|---|---|
| median lateness | 64 samples | 0 samples |
| p95 | 128 samples | 128 samples |
| worst sample | 1984 (41.3 ms) | 2048 (42.7 ms) |
| `latenessMax` | 1984 (41.3 ms) | 2816 (58.7 ms) |
| samples over one tick | 45 of 1200 | 50 of 1200 |
| `rtViolations` | 0 | 0 |

**GREEN again** - the window's worst case is 832 samples (17 ms, 0.87 of a tick) above the baseline's,
inside the threshold, and its median is lower, which is noise. **But the finding here is not about
the window at all.** The spikes arrive every two seconds in BOTH conditions, which is the GO cadence
exactly: **a GO on a five-hundred-cue show costs about twenty-five milliseconds of tick lateness**,
and roughly four percent of samples exceed one tick while cues are firing, with or without a client
attached. That is the engine's own cost of spawning and ending runs on a show that size, it predates
every line of the compiled client, and it is what the window's contribution has to be read against:
seventeen milliseconds of client beside twenty-five of engine. It is not a violation - `rtViolations`
is nought throughout, lateness is not a glitch, and a launch is scheduled `launchLatencyTicks` ahead
precisely so that being late by a tick does not make a cue late - but it is the number to watch, and
the one a real-device take should quote. **Recorded as an engine observation for the author rather
than acted on:** nothing in Phase 5 asked for it, and a number nobody has decided is a problem is not
a problem to fix at five in the morning.

**The first two takes were both wrong, and how they were wrong is the useful part.** Take one read a
flawless zero for every reading in B and printed GREEN - because the window had hung before the
clock started (§14.16's 233 kB of warnings) and every sample was a failed HTTP read falling back to
a default of nought. An instrument that cannot tell *nothing went wrong* from *nothing happened*
says the thing you hoped for; it now refuses to grade a condition whose clock never ran. Take two,
with the hang fixed, read **2.5 seconds** of worst-case lateness against 16 ms and printed RED - and
the plan's remedy for RED is to make `elevateCurrentThreadForTicking()` real. **Acting on that would
have been correct work on the wrong fault.** The median and the p95 were bit-identical with the
window open, and once the instrument printed WHEN each stall happened the shape was plain: four
samples at t+0.1 to t+0.4 s, decaying 2254 → 1686 → 1137 → 496 ms, then nothing for 119.6 seconds.
That is a tick thread working through a backlog, not one being starved. The audio clock was started,
then the window took two and a half seconds to build, and only then did `ticks.start()` run - so the
thread inherited a hundred and twenty-five ticks it had never consumed and reported exactly that.
The window is now built before anything counts samples (`Console.cpp`, and the comment there is this
paragraph in short). **A red that a statistic cannot explain is a red to look at, not to act on;
and a measurement has to be able to say when, not only how much.**

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
instead. Playwright when the page has stopped moving, if at all. *(Corrected by PR 5.18,
2026-09-15: the reconciler works on the DOM and is not a pure function over a map, so it is
tested on the least DOM it touches, where what it changed can be counted (§14.3). No workflow
installs a JavaScript runtime even now: `console.unit` runs on the Node each runner image
already carries.)*

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

**The second GO gesture.** The author decided on 2026-09-16 that starting a group from the member
the pointer is on is a gesture of its own, and that GO stays *fire the one cue the pointer is on*
(decision X). What is built is the pointer half — the standby may stand inside every group, and
▲/▼ walk it from there (§12.6). The gesture is not: §12.6 proposes `go.from` and names three
things about it that are the author's rather than this document's, one of them being what *from
here* can even mean in a timeline group, whose members are simultaneous — which is §3.13's
`list.aim` and a load-to-time under another name. A command whose meaning in one of the three
group modes is still a question is better named once than renamed.

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

**AND IT NOW HAS ITS FIRST JOB THAT THE PAGE CANNOT DO AT ALL (decision Y, 2026-09-16).** The
author asked for two gestures — a media file dropped into the cue list making media cues, and one
dropped onto a media cue linking it, with a confirmation before an existing file is replaced — and
both were deferred here rather than built. The reason is not scheduling. **A browser never tells a
page the path of a dropped file**: it gives the name and the bytes and withholds the rest,
deliberately and by design, so a web client cannot express *"use the file where it sits"* at all.
It can only offer to copy one into the show, and the engine has nowhere to receive it —
`OscQueryServer` answers `GET` and refuses every other method, and there is no import command. A
JUCE file drop hands over a real path.

That makes media import the first thing on either client's list that is not a matter of which one
was written first, and the evidence §9's question E has been waiting for is now one item closer:
not a hole the engine must open for a second client, but a capability the FIRST client cannot have.
Whatever is built, `media/@file` stays relative to the bundle, so an import is a COPY however it is
reached — which means the engine will need an import path in the end regardless, and the desktop
client is simply where the gesture can begin. §9's decision Y lists what is open about it.

**§9's QUESTION E IS ANSWERED, AND THIS OUTLINE IS WRITTEN AGAINST THE ANSWER** (settled
2026-09-17; the argument is in §9 under E). The client runs **IN PROCESS**, for the author's two
reasons - media, which decision Y established a browser cannot do at all, and less code than a
second networked model - with the page and a remote app complementing it for the times somebody is
not at the machine.

**AND IT GETS AS MUCH SLACK AS IT NEEDS.** The desktop client is the richest surface this project
will have; the page is a useful subset of it. That is not a compromise struck between them, it is
the shape the author already runs: WFS-DIY's Android remote drives sixty-four inputs and ten arrays
over bi-directional OSC at fifty hertz, and its desktop plugin has far more than the tablet does.
The tablet is a subset of CONTROLS, never of POWERS. So nothing in this outline holds the compiled
client down to what a browser can draw, and the page lacking a view is never a reason for the
engine to lack a command.

**What this subsection had wrong was not its answer but its question.** It argued that an
in-process UI is *"a client with a shortcut available to it"*, and that the shortcut erodes PRD
§3.2's law on the day somebody takes it because a round trip was inconvenient and the document was
right there on the same heap. That hazard is still real and is watched for below. What does not
follow is that a second PROCESS is the only guard - nor, as the same paragraph implied, that every
client must be able to do everything every other client can.

Read whole, §3.2 says so itself. The slogan - *"nothing the UI can do that the API cannot"* - is
followed at once by the sentence carrying the content: *"Every gesture-reachable **action** also
exists as a named command… Modifiers and gestures are an accelerator layer over a complete command
set, never the only route."* The binding word is ACTION, and the page has been proving the reading
since Phase 3: it has sliders, number boxes, keyboard shortcuts and a drag-to-reorder, not one of
which exists in the API, and not one of which anybody thought broke anything. They are ways of
producing an argument, and they end in a named command.

**So three things, and the third is the only one that binds:**

1. **What belongs to a machine stays with that machine.** WFS-DIY's remote has *Find Device* -
   flash the screen, sound an alarm, locate the tablet in a dark venue - and a finger-pressure
   calibration, neither of which exists anywhere else and neither of which touches the audio. A
   native file dialog is this client's *Find Device*.
2. **Copying media into the bundle is not a change to the show.** The decision is the cue naming
   the file (§4.10); the bytes arriving in `media/` are a fact about the disk, like the timbre
   cache the analyser thread already writes with no command and no record. So the client may open a
   dialog, copy the file in itself, and send one ordinary `node.set` for the cue to name it.
3. **The show changes only through named commands** - which is what puts a change in the undo
   history, in the event log, and in a replay of the night it went wrong.

**How it reaches the engine, which E's answer makes a smaller question than it looked.** Both doors
already exist and are already the ones OSC arrives through: `Engine::submit (origin, command,
args)` and `ParameterTree::snapshot()`. Those cover changing the show and reading it; the file work
above touches neither, since the bundle's path is published like anything else. The client
therefore needs no `ShowDocument&` that anybody can presently name - recorded as an observation,
not a prohibition. If a day comes when it does, that is worth stopping over: it means the API is
missing something the tablet is missing too, and the repair is a new command rather than a reach
past the door.


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

**The client layer, which E's answer makes smaller rather than larger.** *(Rewritten 2026-09-17.
What stood here drew a SECOND PROCESS: `wfg::client::EngineClient` polling `GET /godot` on a
client thread through Go.dot's own `oscquery::OscQueryClient`, a WebSocket from juce_simpleweb's
client side, and a page of argument about `OscQueryClient.h:46-51` — "IT BLOCKS, AND IT MUST NEVER
RUN ON THE TICK THREAD… MountProbe is what owns the thread this runs on" — and how `wfg-client`
would have to become a sanctioned second owner of that rule. **None of that is needed now, and
saying so is the point: an in-process client is the SMALLER thing.** No HTTP poll, no WebSocket, no
second copy of the tree, no blocking client thread and no rule to set aside. The paragraph is kept
in this form because a reader who finds `wfg-client` in an older draft should know it was
considered and why it went.)*

What replaces it is two calls, both already there and both already what OSC arrives through:

- **Reading** is `ParameterTree::snapshot()`, a `shared_ptr<const TreeSnapshot>`. The UI takes a
  copy per repaint - a pointer copy, no lock held while it draws, and no possibility of seeing a
  half-applied tick, because the snapshot is only ever swapped whole. This is the same object the
  HTTP server answers `GET /godot` out of, so the two clients are reading the same thing and the
  page's is merely a serialised copy of it.
- **Writing** is `Engine::submit (origin, command, args)`, which puts an event on a queue that
  never blocks its caller and is applied on the tick thread in arrival order, logged and replayable.
  The `origin` a desktop gesture carries is what a log reader needs to tell it from a datagram, and
  it costs a string.

**The thread shape is forced rather than chosen**, which is why the author's *"a threaded client
would be preferable"* and JUCE's own requirements do not have to be reconciled: a JUCE component
may only be touched on the message thread, the document may only be written on the tick thread, and
the queue and the snapshot are exactly the seam between those two. The UI never blocks the tick
thread and is never blocked by it. `HostPlayer` and the OSCQuery server already sit behind the same
seam, so this adds no new threading rule to the engine at all - which is the strongest argument for
it and was invisible while E was assumed the other way.

**The cost of the answer, which nobody had written down while E was open.** A separate process can
crash without taking the show with it: a page that throws is a page that gets refreshed while the
cues keep running, and that has happened during this phase's own development more than once. **In
process, a client crash takes the engine down** — the audio stops, the runs die, and the operator
is left with a dead machine mid-cue. That is not an argument against the answer, because the
answer was taken on media and on code size and both still hold; it is the thing the answer costs,
and it is written here so that it is a known price rather than a discovery. Two mitigations are
adopted with it: **the window is off by default**, a flag on `serve` rather than the shape of the
binary, so every black-box driver and every headless deployment is unchanged and a suspect client
can be left out of a show entirely; and **the page stays live**, which makes it the redundancy
path PRD §3.17 already says it is, on the night the compiled client is the thing that failed. What
this buys is a real choice at the desk — if the window is what broke, close it and run from a
browser — and it is only a choice for as long as the page can still run a show, which is a
standing obligation on this project and not a temporary one.

**§14.2's contract is unchanged, and it is narrower than it has been read as.** Two clients, one
contract, neither with a door into the SHOW that the other lacks - which is not the same as two
clients with the same controls, and never was. The desktop will have views the page never grows,
and a file dialog the page cannot have at all; what it will not have is a way to change the show
that leaves no record. What kept that before was a process boundary. What keeps it now is the
third of E's three points - the show changes only through named commands - and it is checkable
rather than trusted, because a gesture that reached the document directly would write nothing to
the log and `wfg replay` of that session would not reproduce it.

**And the contract needs one rule it did not need while E was open, because the slack above would
otherwise quietly end the page's redundancy.** *Every command the desktop sends stays reachable
from the page's generic inspector.* The desktop is free to have views the page never grows — that
is settled above — but a view is a way of producing an argument, and the argument has to remain
producible by somebody who only has a browser. The case that shows why is the smallest one:
*mark as preset* is a select of ancestor groups on the desktop and an ordinary `node.set
…/preset <group>` underneath, so the page reaches it through the inspector it already builds out
of `TYPE`, `ACCESS` and `RANGE` (§14.2) and nothing has to be written for it. That is the shape to
keep. A desktop-only view that is the ONLY route to a command is the failure, and it fails PRD
§3.17's *"the redundancy path (§3.5), and a redundancy path that requires an installed app is not
one"* rather than §3.2 — which is why this rule sits here and not with §3.2's three points above.
It costs nothing today because the generic inspector is generic. It stops costing nothing on the
day a command is added whose arguments the inspector cannot express, and the answer then is to fix
the inspector or to reconsider the command, not to let the page fall behind.

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

**What M1 built (2026-09-17).** `wfg serve <bundle> --window [--theme=<file>]` opens the compiled
client over the engine it runs inside: one transport strip - the show and the word *unsaved* beside
its dot, the tick, the clock and the rate, the focused list and the cue its standby stands on, the
audio status, the last refusal in words - a GO button, and Space. Off by default, so nothing
headless changed. Three things the outline above did not have and the tree forced. **The engine
does not link the client**: `Console.h` takes the window as a vendor-free factory handed in by
`main()`, because the client library links the engine and the arrow cannot point both ways, and a
build that hands over no factory answers `--window` with a sentence. **JUCE is initialised first in
`runServe`** - a `ScopedJuceInitialiser_GUI` as the verb's first declaration - because the message
thread is whichever thread first reaches `MessageManager::getInstance()`, and until then that was
the loop helper at the bottom of the verb, after the point where a window would be built; the macOS
`initialiseNSApplication()` moved out of that helper for the same reason. Headless is untouched, as
the selftest verb has shown since Phase 0. **The look is not compiled in**: `clients/desktop/theme.json`
carries the page's tokens with the page's values, `--theme` lays it over the defaults and F5 re-reads
it, with a refusal landing on the window's own status line - the author's rule, that a visible thing
is editable or it earns one round. The split is two static libraries, `wfg_client_model` (std-only:
what a cell shows, the transport reading, the theme, each gesture's Event - asserted under both
locales with no window, in `ClientTests.cpp`) and `wfg_client_ui` (built by every job, run by none),
held apart by `scripts/check-client-boundary.py`: no `childrenOf`, nothing the tick thread owns,
exactly one `snapshot()` call site, no `juce` in the model - rule 2 as a ctest. The origin is
`window`, beside `cli`, `replay` and `engine`, and `window-go.wfglog` is the first fixture carrying
it. The close button asks before stopping the engine and refuses while the show is locked, which is
the price of E's answer paid at the one gesture that incurs it. What could not be tested is written
into `ui/Client.cpp` as such - that the window opens, layout, colour, hit-testing, focus, the
dialogue, timing, the shutdown order - and the first person to find a broken window was the author,
on this build.

**What M2 and M3 built (2026-09-17, overnight).** M2 gave the window the rest of the transport -
undo and redo under the engine's own name for what they would take back, save, revert, the lock,
and the recovery offer as a banner that is itself the question - and M3 gave it the cue list. Three
things came out of building them that were not in the plan. **A flag has three answers.** A node the
engine has not published is not a node reading false, so `Flag::unsaid` sits beside `no` and `yes`,
every gesture asks `isYes`, and every sentence has a word for the third - the page's rule, moved to
where both clients' readings are made. **Every gesture is checked against the real registry**, which
is `ClientTests`' version of what `client_page.py` does for `commands.json`: each one names a command
`wfg commands` lists and sends arguments its signature accepts, asserted against a registry built the
way serve builds one. **And the cue list needed a gesture the plan had put in M5.** `standby.next`
stays put *"from nowhere"* - the engine's own wording and its own decision - so a show whose
`state.xml` names no standby has no keyboard route into it at all; the page answers that with a click
on a row, and until M5 gives a click its second meaning, so does the window. The list is a `ListBox`
over `model::ShowModel`, which walks the show only when `/godot/document/revision` moves or the
focused list changes, and the rule M0 exists for is now a counted test: a hundred publishes with
nothing applied rebuild once, a standby move rebuilds nothing, an edit rebuilds once more.

**THE WINDOW HUNG ON A SHOW WITH 1,824 THINGS WRONG WITH IT, and the instrument called it GREEN
(2026-09-18).** M25's first take opened `--window` on a generated 500-cue show, reported a flawless
zero for every reading in condition B, and declared the window free. It was not free: it had hung
before the clock was ever started, every sample was a failed HTTP read falling back to a default of
nought, and the tick read zero because `ticks.start()` comes after the client is built.

**The cause was mine and it is a rule worth stating.** `/godot/document/warnings` is one line per
thing wrong with the show that did not stop it opening; on that bundle it measured **233,471
characters over 1,824 lines**, because a generated show names media files that are not there. The
transport carried that string whole in its reading, compared it field-wise twenty-five times a
second, and handed it to a `juce::Label` one row high. Laying a quarter of a megabyte of text into
thirty-two pixels is work without end - a spin at a hundred percent of a core, before the window was
ever visible. `phase4` has almost no warnings, which is why every earlier session was fine.
**A client never hands an unbounded engine string to a fixed-size control.** The reading now carries
a count and a first line, both bounded (`countWarnings`, `firstWarning`, declared so a test can hand
them a quarter of a megabyte), the label clips whatever reaches it, and a view with room can read
the node itself.

**And the second fault is the one to keep.** The instrument could not tell *nothing went wrong* from
*nothing happened*, so it printed the answer somebody hoped for. It now refuses to grade a condition
whose clock never ran, and says every reading is a default rather than a measurement. §14.14's idiom
is that a measurement asserts nothing; what this adds is that it must still be able to say it
measured nothing. The diagnosis took four experiments - bisecting by cue count, which misled,
stubbing the row count, which cleared the cue list, and finally switching the two panes off one at a
time, which named the transport in one run. The switches were scaffolding and are gone; the
technique is the part worth keeping.

**A FOURTH RULE FOR THE CLIENT, bought by the author's first session with the cue list
(2026-09-18).** They clicked two rows and got `error: 5411 26 window not-a-stop standby.set` where an
answer should have been. The engine was right: `P4MSG002` is inside a `Footer` and `P4MSG003` inside
a `Persistent` section, and decision X lets the pointer stand on any cue of its list EXCEPT a header,
a footer or a persistent one - each of those runs with its group or from the top of the show, and
none is a place anybody waits.

**What was wrong was the window offering the gesture.** It drew those rows, correctly, and let a
click reach them. So: **a client does not offer a gesture it could have known would be refused.** The
rule lives in the model - `Row::mayPark`, where a test reaches it - rather than in a click handler,
and a row that cannot take the pointer now says which of the three reasons applies, because *nothing
happened* and *this is not that kind of row* look identical from a chair. The page has the same
shape available to it and §14.3 should take it when Didi next moves.

**And a refusal is a sentence, not a log line.** `/godot/engine/lastError` is five fields written for
`grep` at four in the morning - tick, sequence, origin, reason, command - which is right for a log
and wrong for a strip: somebody who has just pressed a thing needs what and why, and the tick is
noise because they were there. The window shows `standby.set refused: not-a-stop` and keeps the whole
record one hover away. **The reason word is not translated**, and that is deliberate: a table of
friendly sentences in the client would go stale the day a reason is added by somebody who never
opened that file, which is the argument `undoName` already makes. Anything that does not parse into
five fields is shown whole, so a format change is visible rather than swallowed. Whether the reason
codes themselves should read as English is the author's, and is open.

**WHAT THE AUTHOR'S FIRST HOUR WITH THE WINDOW CHANGED (2026-09-18).** They opened it, used it, and
said four things; all four are built and three of them were the plan's later milestones brought
forward, because a layout cannot be judged one pane at a time.

- ***"Selection on the full line sets the stand-by and not the far left of each row."*** Park is the
  left gutter now, where the pointer's own mark also moved. The rest of the row is deliberately
  inert: it becomes SELECT when the inspector arrives, and a gesture taken back from a whole row
  later is one somebody will have learned by then. `commands.json` has said *"a row's left edge"*
  since Didi was drawn.
- ***"Containers are not as clear as on the webview."*** They were indented and nothing else.
  `styles.css`'s shape is now the window's: **a rail and two corners and never a box** - a recessed
  ground, a one-pixel rule down every contained row, a container starting that rule under itself,
  and the last row inside turning it right and stopping it. The author's word for the result was
  *"filetree view"*, which is the shape a file tree has taught everybody already.
- ***"We're missing the running cues and the Inspector."*** The running pane is built, beside the
  cue list: the state as a mark for the two an operator recognises and a word for the six they read,
  the CUE's name rather than a run identifier nobody knows, the position, and a failure's reason in
  place of the name. A cross at the right edge kills one run - the mirror of the park gutter, and
  for the same reason. **It is read fresh every pass and the cue list is not**, which is the whole
  of the two-rate design in one sentence: runs have no revision to key on, because a run is not a
  decision anybody recorded (§4.10).
- ***"Are the header and footer sections already present? I think we need a special container for
  the persistent cues that can be folded or expanded."*** They were present as ROWS and not as
  FRAMES, which is exactly why the question arose. All three sections are bands now, and they fold:
  a twist, the word and a count - three tellings and not one a colour (§4.8) - with the count
  becoming *"N hidden"* when shut, so a folded section still says how much is behind it. **The fold
  is the client's and never the engine's** (§14.1): it never reaches `submit`, and it survives an
  edit because the set is keyed on the container rather than a row's position. It does not yet
  survive a restart; the page keeps that in `localStorage` and the window will want a small file.

**And the inspector is what M5's selection is for**, which is why the rest of the row stayed inert
until it existed. It was the next thing the author asked for and the next thing built.

**WHAT THE INSPECTOR ROUND SETTLED (2026-09-18).** The panel is built from the tree and from no table
of field names: the node's type decides the control, its range the bounds, its closed set of values
that it is a choice at all, its description the hover. A row added to the parameter table appears
there with no line written in the window, which is what §14.2 means by generic and what the page
proved first. The block order is the page's, transcribed as data because the author settled it there
with the page open, and a window that re-argued where `preWait` goes would be two clients disagreeing
about one panel. A click now carries two meanings, split the way the page splits them: **the gutter
parks, the row picks**, drawn apart because they are different questions.

Four things came back from the author using it, and one of them was a real bug with a plain cause.
The fields wanted a DOUBLE click while the dropdowns opened on one, so a loop count typed into a
number box was never sent - no editor had opened - and they reported the loop count broken. It was
not; but a panel whose fields need a different number of clicks depending on their type is a panel
nobody can learn. **`loops` is three controls now** - loop, for ever, and how many - because one
integer was carrying three questions and a box showing `0` answered none of them; one `node.set`
underneath, unchanged. The greys are lighter and the inspector is blue, which diverges from the
page's hex on purpose and says so in the theme file: a browser and JUCE do not lay type down the same
way, so identical numbers kept the look identical only on paper. **And the folds did not fold**: the
model folded correctly and the view keyed its rows on the SHOW's revision, which a fold does not move
- nor should it, since collapsing a section is not a change to the show. The view keys on the walk
count now, which moves for every reason the rows can change, and a test counts walks.

**THE END OF A SHOW CLEARS THE POINTER, and the arrows do not (2026-09-18).** The author, watching
the window: *"once the last cue of the show has been triggered and the standby has no other cue to
go to, it should be cleared."* An engine change, not a client one, and PRD §3.5 carries it.

**What it costs to leave it.** The pointer used to stay on the cue that had just gone, so a second
GO at the end of a show **fired that cue again** — which is not what anybody pressing GO once more
means. Cleared, the second GO is applied and does nothing, which is precisely what the `go` handler's
own comment has claimed since it was written: *"GO with nothing in standby is applied and does
nothing. An operator at the end of a list has not made a mistake."* The change makes the code agree
with the comment rather than the other way round.

**AND IT IS THE RESTING STATE, not a new one** (§4.6). An empty pointer is nowhere at all, is always
legal (§3.5), and is what a list carries before anybody arms it — so a show that has been run through
ends where it began rather than in a state of its own.

**THE ARROWS KEEP THE OLD ANSWER, and that is the whole reason this is a second function rather than
an edit to the first.** `standbyAfterFiring` sits beside `nextStandby` in `cue/CueList.h`, and only
`go` calls it. Walking off the end is somebody LOOKING, and a pointer that vanished under a keypress
would be the machine taking their place away; firing off the end is the show being over. Getting a
cleared pointer back is a click on a row, which both clients offer, because stepping from nowhere
stays nowhere — the rule that made click-to-park necessary at M3 in the first place.

**What the change forced, and it is the better half of it.** The manual-group rounds rule — the one
question the cursor asks about what is RUNNING rather than about what is written — was inline in
`nextStandby`, and a second walk asking it would have been a second copy of thirty lines of argument.
It is now `heldForAnotherRound`, named once and asked by both. Without that, a one-member manual
group with rounds left, standing last in its list, would have had its pointer cleared out from under
a scene with two thirds of itself still to play — a fault the shape of the refactor prevents rather
than a case a test happened to catch.

**AND CLEARING IT HAD A CONSEQUENCE, which is the better half of the change.** §3.29's persistent
section asks the solver what should be sounding, and the solver reads the pointer to know how far
through the show everything is. Its rule for an empty pointer was *"a list nobody has parked on is
the top of the list, so nothing has happened and nothing is suspended"* — true while empty could only
mean *never armed*. After this change it also means *ran out*, which is the opposite fact, and a show
played to its end would have re-asserted every bed a Stop had killed: the ambience coming back on
after the last cue, with nobody on the GO button.

**It was a unit test that said so** — `persistent: a stop before the pointer suspends it` — and it
said so within a minute of the change, which is the whole argument for the seam tests this phase
keeps writing. The fix is a second fact, `list,finished`, kept in `state.xml` beside the pointer it
qualifies: set when a GO fires the last cue there is, cleared by any `standby.set`, and read by the
solver as *past the end, so everything counts*. Two things the building of it taught, both cheap and
both the kind that waste an hour: the document's own door **refuses a read-only attribute**
(`reason::readOnly`), so an engine-only row still has to be `rw` to be written at all; and a `T` row
lands in the tree as a **bool var**, whose `toString()` is `"1"` and not `"true"` — the flag was being
written correctly and read as false for a whole build.

**THREE THINGS CAME OFF THE TRANSPORT THE SAME AFTERNOON**, all asked for by the author *"to gain a
bit of headroom"*, and each replaced by something that says the same thing where somebody is already
looking.

- **The undo and redo sentences** are the buttons' tooltips now (`undoTip`, `redoTip`), still the
  engine's own name for what would be taken back and still with a word for a flag that is `unsaid`.
  A history panel listing what can be undone and redone is the author's own suggestion for when the
  line is missed.
- **"Audio running" is gone**, to a configuration panel nobody has built yet. Whether a device is
  open is something somebody sets up once and then stops reading. The **lock keeps its word** on the
  same line, because §4.8 will not have it carried by a colour alone.
- **"Saved" is gone too, and the Save button dims instead.** A state a control can BE in beats a
  state a control is described by. `unsaid` still offers the save: a dot the engine has not published
  is not a show with nothing in it, and offering a save that turns out to be unnecessary costs a
  write while withholding one can cost an afternoon. The two reasons a save is not offered — a
  locked show, and a saved one — are told apart by the tooltip.

**M11 CAME FORWARD, AND WITH IT A SECOND READ DOOR (2026-09-18).** The author, with the window
open: *"can we have the waveform beneath the media cues in the active cue panel with a progress bar
showing where the playhead is? Pre-waits and post-waits can also have progress bars, maybe running
the opposite way, right to left, as a countdown."* That is §14's 5.17 and it arrived six milestones
early, which is what M2's lesson predicted: what the author asks for after looking is never the next
view on the list.

**NOTHING NEW IS MEASURED.** §3.30's analyser already computes what every file sounds like, halved
level by level down to sixty-four frames, and `Timbre.h` already says what a client does with it —
*"a client drawing a bar picks the level whose frame count is nearest its pixel count and reads it
once, so no redraw recomputes anything"*. `client/model/Waveform.h` is that sentence and nothing
else: pick a level, bucket it into columns, stop. A window that decided for itself what a file looks
like would be a second answer to a question the engine has answered, and the two would drift the
first time the ramp moved.

**THE PYRAMID IS NOT IN THE PARAMETER TREE AND NEVER WILL BE**, which is why this needed a decision
rather than a lookup. It is tens of kilobytes per file; `GET /media/<hash>/timbre` exists because a
page must fetch it over a socket. A client in the same process does not have to, and asking it to
would be a socket opened to talk to itself. So `ClientHost` carries the analyser's table, and
§14.16's second rule now reads **one call site per published snapshot** rather than one call site
full stop — the boundary gate checks exactly that, and names each door in its output. It is not a
reach past the tick thread, which is what the rule is actually about: `MediaInfo::snapshot()` is an
immutable table the ANALYSER thread publishes under a short mutex, and the HTTP thread has been
reading it on every request since PR 5.8.

**ONE ENGINE NODE WAS OWED AND IS NOW THERE.** A countdown needs to know how much of a wait is left,
and nothing published it: `run,remaining` is a subtraction from `dueTick`, which a handler set from
its own tick, so a client that missed twenty publishes reads the truth on the next one and nothing
accumulates. Nought whenever the run is not waiting, so no reader has to ask the state node whether
this one means anything.

**AND THE STRIP UNDER A RUN IS ONE OF THREE THINGS, never two at once.** A wait is a bar in one
colour whose MOTION says which wait it is — a pre-wait empties right to left and arrives at the cue,
a post-wait fills left to right and is full when the run is done, which is why one hue is enough and
why a hue would have been the weaker telling anyway (§4.8). A sounding media cue is its own waveform,
amplitude in the column heights and timbre in their colour, with the playhead on it. Anything else —
and a media cue whose analysis is not built yet — is a plain cursor on a shaded ground, because a
waveform that is not ready is not a cue with nothing happening. A fade carries its own colour
wherever it appears, being the one kind that changes something already sounding rather than starting
or ending anything.

**WHAT AN HOUR OF THE AUTHOR'S EYE CHANGED IN THE CUE LIST (2026-09-18).** Six things, and the first
of them was a fault the others made visible.

- **The bracket did not meet the twist it came from.** A section's band and the rows it heads were
  measuring their rails from two pieces of arithmetic that agreed at the top level and nowhere else.
  They now measure from one function, `railsOrigin()`, and the gate against their drifting again is
  that there is only one of it. The twist is CENTRED in a cell exactly one indent wide, whose centre
  IS the rail its children come down — so the tip of the triangle stands on the line, with no second
  sum to keep in step.
- **A section's rows sit one level in**, like a group's, which is what gives a header and a footer
  the bracket they lacked and the band a rail to open. They had shared their band's depth, so
  nothing drew them as contained.
- **A header, a footer and a persistent cue have their own tone**, and it took four tries to land:
  a hair cooler than the panel, which read as nothing; lighter, which the author turned down; black,
  which they then asked to invert. The final word is **the list's own rows black and a section dark
  blue-grey**, band and rows alike — black is where the eye rests, and a section is the thing that
  differs. The running pane is black too, which is also where a waveform's colours have most to stand
  against. Three theme tokens carry it, so the next change is a line in a file.
- **Nothing alternates.** A zebra is for following a row across a wide table, and it was fighting the
  two distinctions above — which carry meaning, where a stripe carries only parity.
- **A group's name is larger and carries its behaviour as shapes**: a loop mark with its round count,
  an infinity for a group that loops for ever, and a shuffle mark. Every one of them is also a word
  in the inspector, so none is the only telling.
- **The column labels live outside the list**, drawn by the component rather than by a row, so they
  stay put while the show scrolls under them. They take their widths from the same three constants
  the rows take theirs from, which is what stops a label and its column coming apart.

**And GO moved to the left, with the cue it will fire beside it** — the hand goes to one place and
the eye reads outward from it, rather than reading a name and travelling back across the window.

**A FADE'S PLAYHEAD NEVER MOVED, AND IT MADE AN EDIT LOOK IGNORED (2026-09-18, afternoon).** The
author changed a fade's duration in the inspector, ran it, and reported that the duration had not
changed. Their own log said otherwise — the `node.set` was applied and the fade's run lasted exactly
the 150 ticks asked for — so what they had been reading was the bar under the run, and the bar had
not moved because `run/position` had never been computed for a fade. `Runner::updatePositions`
measured every playhead from the SAMPLE a launch was placed at, and a fade holds no voice, so
`launchedAtSample` was nought for it for ever. **Two reports, one cause**: "the fade's progress bar
isn't moving" and "changing the duration doesn't change the fade" were the same fact seen twice.

**The repair widened what `position` means, and the parameter table says so.** A run with no voice is
measured in ticks from the GO that started it; a run with one keeps the sample clock, which is the
finer answer and the only one a range can wrap. `run,position` reads *how far into the RUN*, which is
the file only when there is one. The stamp it measures from — `launchRequestedAtTick`, which the
runner already kept to measure lateness — had been set on the media path alone, and the first repair
put a second stamp in `fireNow`, which is only the path a cue with a pre-wait takes; a cue without one
is fired straight from the GO and never goes near it. It lives in `fireKind` now, the one place every
kind passes. A test pins that a fade's playhead advances.

**And a second engine reading, because the running pane needed it.** `run,started` is that same
launch tick handed out, so a pane can be ordered the way the show happened rather than the way the
run table holds it: a cue the anticipation window prepared is CREATED before the things already
sounding, so engine order put the next cue above them.

**WHAT PARKING THE POINTER DOES, restated because it was reported four times.** `phase4`'s "Position
the input" is a header-derived preset line — `preset="P4GRP001"` — on a mount declared
`anticipatable` with an OSCQuery readback, and it waits for verification. Parking on the group is
§3.12's prepare: the write is pre-sent so GO commits only the perceptible part, and a readback is
awaited for the cue's own `timeout`. The fixture declares a desk at a port nothing is on, so the
pre-sent write fails `timeout` five seconds later, every time, correctly. The author's model —
parking *"shouldn't do more than place the pointer [and] run the contents of the preset
container(s)"* — is exactly what the engine does; what they objected to was a preset line FAILING,
and that is the fixture's missing desk. Two things learned making the fixture quiet for them: the
cue's own `wait` does not govern a pre-sent write (`wait="sent"` alone still verified and still timed
out), and `wfg validate` refuses `readback="none"` beside a cue that waits for verification — *"a cue
that cannot succeed is worse than one that fails"* — so both have to change together. Nothing in the
engine changed for this, and nothing should: a pre-sent write that is failing is what an operator
needs to see.

**A SAVED SHOW REFUSED EVERY FEED IN IT (2026-09-18, afternoon), and the author found it by parking
on a cue.** *"When the standby pointer lands on 2 it shows in the active cues as an error bad
route."* Cue 2 feeds a processor input. The window had been restarted with `--recover` to keep the
edits made that hour, and the recovered show — written by the autosave — had no `width` on either
slot: the canonical writer omits an attribute that equals its default, and a slot one channel wide
IS the default. `ShowDocument::getAttribute` knows this — *"an absent attribute IS its default; that
equivalence is what lets the writer omit defaults and still round-trip"* — and every cue attribute the
runner reads goes through it. `resolveRouting` did not: it read a bus's and a slot's `width` and
`firstChannel` straight off the tree, where an absent width is nought and nought is *"the slot does
not fit in its bus"*. **The autosave and `document.save` are one writer**, so this was not a recovery
bug: any show saved since PR 4.2 and reopened would have refused every feed in it, and the fixtures
never showed it because every one of them writes its widths out by hand.

**The repair is the one the rest of the runner already made.** The schema `Reader` in `ShowWalk.h`,
which applies the parameter table's defaults for the cue-side owners, learned `bus`, `processorInput`
and `rackChannel`, and the four reads go through it; the runner's own file-local `idProperty` went at
the same time, since the reader's header carries the one there is. A regression test builds a slot
and a bus exactly as the writer would have left them — the bus named, nothing else — and routes a
feed and a route through them. **Recorded as a rule for the reader of this file: a raw
`node[juce::Identifier (...)]` on a document attribute is a latent bug wherever that attribute has a
default**, because the writer will drop it and the tree will not put it back.

**The pane draws armed runs.** They were hidden for one build on a misreading of that report, and
the author's words put them back: *"I don't mind seeing the armed, preloaded cues ready to fire."*

**WHAT M7 BUILT, AND IT IS THE ONE THING THE PAGE CANNOT BE GIVEN LATER (2026-09-18).** Decision Y,
in a gesture: a file dragged onto the window. A browser is handed a dropped file's NAME and BYTES and
never its path, deliberately and by design, so it can only ever offer to upload one. This is handed
the path, and that is the whole argument for compiling a client rather than serving it.

**An import is three things and only two of them are the show's** (§14.16). The bytes arrive in the
bundle's `media/`, which is a fact about a disk - like the timbre cache the analyser writes with no
command and no record - and the window does that itself. Then a cue is created and the cue names the
file, which are decisions, and go through the one door as `cue.create` and `node.set`. **The client
does not draw identifiers**: `cue.create` takes an optional id and that argument is for REPLAY, so a
create is followed by FINDING what it made, at the member position it was asked for. There is exactly
one entropy consumer in this project and a window is not going to become the second.

**Two gestures, told apart by what is under the pointer, and said while the hand is still in the
air.** One file onto a media cue NAMES that cue's file and the whole row lights; anywhere else makes
cues, one per file, and a line is drawn under the row they will follow. Guessing afterwards which of
the two happened is the thing the feedback exists to prevent. A replacement asks first, and the
question says which of two things is at stake - the cue's current choice, or bytes of the same name
already in the bundle - while a drop with neither at stake asks nothing, because a dialogue nobody
needs is one people learn to dismiss unread.

**Three faults the building of it found, none of them visible from the drawing.**

- **A display row is not a member.** The first draft took the index from the row the hand was over
  and, for a drop into empty space, from the number of rows drawn. Those rows are what is DRAWN -
  bands, and the members of every open group - so the count names a position inside the list rather
  than its end. For ONE file it would have worked; for two it would have quietly mismatched cue and
  file, because every create beyond the end lands in the same place. The index is resolved in the
  window, against the order the show actually has.
- **A header, a footer and a persistent cue cannot be pointed at.** `cue.create` puts a cue among its
  parent's MEMBERS, and a group's header and footer are separate orders that a role decides. A drop
  on one of those rows says which container was meant and nothing about where, so it goes to the end
  of that container's members - and no line is drawn under a row the cue will not appear under,
  because the promise and the drop are asked in one place.
- **A position alone does not identify the cue a create made.** The show has other clients, and
  somebody inserting from the page inside the same two hundred milliseconds would put a stranger
  exactly where the import is looking. Three things must agree before a file is written onto a cue -
  the name the create was given, a media cue, and no file yet - and a stranger passing all three is a
  cue somebody called the same thing and left empty, where naming it was wanted anyway. Attaching a
  file to the wrong cue is worse than attaching it to none.

**And two engine facts the drop exposes, neither of them the client's to fix.** `MediaInfo::durations()`
is frozen at load and **never grows**, so a cue imported mid-session reads a duration of 0 until the
show is reopened; the file still PLAYS, because the runner resolves it against the bundle at arm from
the cue's own `file` and not from that map. And **`Audio/@tracks` is authored** - the polyphony
ceiling, stated and never inferred (PRD §3.9b) - so a show sitting at zero has nowhere to put a
sound and every run of an imported cue ends `no-track`. That is not a reason to refuse the drop, since
making the cue is a decision somebody is entitled to take and setting the ceiling is the obvious next
thing they will do; so the import happens and the window says what is missing. Which is the
difference between this and the lock: **a client does not offer a gesture it could have known would be
refused**, and a drop into a locked show is refused here, before anything is copied, because bytes
left in `media/` for a cue that was never made are litter nobody asked for.

**A ROW OF NEW-CUE BUTTONS THAT DOES NOT MOVE (2026-09-18).** Until this round the window could make a
cue only by being handed a file. The page's add buttons live in its inspector and follow the pick;
asked whether the desktop should copy them, the author chose otherwise: *"I would place buttons so
people have a stable UI for this. Lock makes them disappear. It also helps getting started."* So the
window has one row over the cue list, one button per kind - memo, media, fade, stop, OSC, MIDI, group -
that is the same row whatever is picked, and that the lock removes rather than dims: a show in show
mode has no way to grow, and reads as one. **Where the cue lands is the page's rule**, after the picked
cue in the picked cue's own parent (a group is a cue, so a new cue lands after the group as a whole),
or at the end of the focused list when nothing is picked; the tooltip says which. **`+ media` is the
native Open followed by the import**, so a media cue made here is never left without its file, which
is the one thing the page's own `+ media` cannot do (decision Y). **The cue is made unnamed and then
picked**: the create is followed by finding what it made, as an import is, and the inspector opens on
it so the name is the next thing typed. Picking is the client's own state, so the guard on that finding
(the kind, and no name yet) costs nothing when a stranger passes it. Rule 3 holds by construction:
every button is `cue.create`, reachable from the page - and building the row from one list of kinds
found that the page's list had stopped at OSC, so MIDI was added there in the same commit. The
inspector's details fold moved in the same round: its button sat at the foot of the pane, a screen
away from the fields it folds, and now heads the detail lines themselves.

**PANIC, AND ESC (2026-09-18).** *"Panic is missing and Esc key is not bound."* Until then no client
had an abort key, deliberately (§14.15's "says nothing about the three levels of stop rather than
implying half of one"), because the engine had only the per-run primitives and a client sending one
`run.stop` per row would have been a gesture with no single record. So the engine gained the two
levels as commands, `run.stopAll` and `run.killAll` (§12.4), each its single-run command over every
root run, and both clients bind them: **Esc** is the graceful abort and the footers run; **Esc again
within 750 ms** is the immediate one and no footer runs. Which of the two a press means is a fact
about a hand, so the client reads it (`model/Panic.h`, one place both the key and the button ask) and
sends one named command for the reading - the log says which level was reached and when, and a
replay reaches the same one. The window counts the second press from the first, so a hammered key is
a stop and then kills, each applied harmlessly to a table already stopping. The desktop has a
**PANIC** button at the far right of GO's row, the same height and as far from it as the row allows;
the page has *stop all* and *kill all* on its running pane's header. What double Esc does NOT yet do
is park mounted parameters at their §4.6 panic values - that is the mount table's, and Phase 10's.

**A ROW DRAGGED IN THE LIST (2026-09-18).** *"Can we have drag and drop reordering? Can we also have
drag and drop onto a fade to set its target? Can we also use its user ID (not only the unique ID) to
set the fade target too?"* The page declined dragging for a reason that holds there - a row moving
under the pointer while the tree is re-fetched - and does not hold in a window with no poll, so this
is the first client to offer it. **Three answers, told apart by where on the row the hand is** and
said on the foot while it is still in the air: the middle band of a fade or a stop means *aim this at
it* (one write to the fade's `target`); the middle band of a group means *into it*, at the end of its
members; everywhere else means *after it*, in the row's own container, which is the reading a dropped
file already gets. The feedback is the file drop's two shapes - a line under for after, the row lit
for on. **The index is `object.move`'s member position in the list as it stands**, so a cue dropped
after a member below it asks for that member's own position and lands directly after it, and one
dropped after a member above it, or from another container, asks for the position after; that
arithmetic is the document's (§14.6's `move`) and the model has one function that speaks it
(`model/Reorder.h`), tested. **A target may be typed as a cue's number or name**, and the window
resolves it to the identifier before the write: the number is the operator's and "never an identity"
(the parameter table's words), so what the document stores is the identity and renumbering during tech
breaks nothing. Two cues with one name resolve to nothing, and the foot says so. Rule 3 holds: the
page's inspector writes the same `target` by identifier, and its ▲/▼ send the same `object.move`.

*The first hour with it (2026-09-18): "Reordering was a bit messy. Moving stuff might have caused
some discrepancies between the displayed order and the playing order."* The log showed five moves,
every one landing where its line was drawn - the arithmetic is now pinned from the client's side by a
test that hands the model's indices to the real `object.move`, in a group with a header so raw and
member positions differ, and reads the order back. The discrepancy was real and was not a fault: the
group was a **timeline** ("Two at once"), whose members start together at entry, each after its own
pre-wait, so their order on screen is not their order in time and a reorder there changes the reading
and nothing else. What was missing was the window saying so. It does now, twice: a timeline group
carries the mark **∥** beside ↻ and ⇄ (the word is the inspector's `mode`), and a drop that would land
in one says on the foot, while the hand is in the air, that its members start together whatever their
order. **Ctrl/⌘-Backspace deletes the picked cue** in the same round, without asking, since undo is
one keystroke.

**A MENU, A FRAME OF ITS OWN, AND A WINDOW PER SHOW (2026-09-18).** *"We need an Open button to
load a project. There can be several windows, each one for an individual project at the same time.
We also need a New button to start a new project. I would put these in a menu in the top bar. Can we
also remove the white window frame for a colour themed one?"* Three things, one of them the engine's.
**The window draws its own frame** in the theme's ground rather than the system's white, with the
resize corner drawn and a floor on its size; the price is the system's frame gestures. **A menu bar
under the title** - File (New show, Open show, Save, Revert), Edit (Undo, Redo, Delete cue), Show
(Lock or Unlock) - with every item a gesture that already existed as a button or a key and ends in
the same one command; its enabled states follow the reading exactly as the buttons do, so show mode
offers no Save from the menu either. On the Mac the same model is the screen's menu. **New and Open
start another PROCESS.** One engine holds one document, and `document.load` was ruled out in Phase 5
as a process restart (§14.10); a window per show is that restart beside this one rather than instead
of it. The console does it, not the client - it has the flags, and a client should not know what a
command line looks like: `ClientHost::openWindow (folder, createNew)` starts `wfg serve <folder>`
with this serve's own arguments less its bundle, ports, log and `--recover`, plus ports the system
handed a probe bound to nought and a log under the user's application data named after the show and
the moment. New writes an empty show first - one list called Main - and wants an empty or absent
folder. **Copy and paste of cues between windows is not built**, and needs a word from the engine
before it can be: a paste is a fragment of one document entering another, which is either N
`cue.create`s and N×M `node.set`s from the client, or one command that takes the fragment - the
second is the one §4.11 would have, and it is not drawn yet.

*Same evening:* the transport's save, revert, undo, redo and lock buttons went, the menu carrying
every one ("we can remove the redundant buttons"); **Save as** joined File, one `document.saveAs` on a
chosen folder; and the menu's keys are the classical ones - ctrl/⌘-N, -O, -S, -shift-S, -Z,
-shift-Z (and -Y), -Backspace, -L - printed beside each item from the one table the window answers
them from, so the menu cannot show a key the window ignores, and a key for an item the reading
disables does nothing, exactly as the item would.

**A FADE THAT STOPS WHEN IT ARRIVES (2026-09-18).** *"Something else I think I haven't seen, a tick box to
stop a media file once a fade has completed."* There was none, and reading the code for it found the
fade cue passing `false` to the one flag that would have done it: a fade never stopped anything, even at
silence - the run played on, silently, until it ended by itself. Right for a fade that will come back
up; a surprise for the common fade-out, whose author then reaches for a stop cue with a fade verb and
finds it is the same ramp under another name. So `Fade/@stopWhenDone` (T, default false, §12) is the
box, read by the fade cue into the flag the stop cue's fade verb already sets - one path, and nothing
new to drift. The inspector shows it on both clients as "stop when done". *Also asked, and noted for the
running pane's next round:* buttons to skip to the next loop or slice of a media run (`run.advance`
exists), and pause and resume (no command yet, and a pause of a run is a design question §3.29 has only
begun to ask).

**SEVERAL CUES AT ONCE (2026-09-18).** *"We're also missing multiple selection and batch editing of
parameters."* The page's 5.12, transcribed. **The selection is the client's** (`model/Selection.h`,
§14.1): a click picks one and makes it the anchor, shift picks everything between the anchor and here in
the drawn order with bands skipped, ctrl/⌘ toggles one in or out, ctrl/⌘-A picks every cue drawn; what
the show loses is dropped from it each pass. **The inspector over N** is the intersection of the rows
every picked cue has and may write, in the first cue's order, with the value they agree on or
*(mixed)* in the box - typing over it writes the one value to all, and leaving it writes nothing; the
reported rows are left out, since a run position is one cue's. **A commit is N `node.set`s**, one per
cue's own address, which is what §4.11 makes a batch edit: N decisions and N records, and N presses of
undo. Delete acts on the whole selection the same way, and the menu says how many. A new cue still
lands after the ANCHOR, the cue somebody clicked last on purpose.

**COPY AND PASTE, BETWEEN WINDOWS (2026-09-18).** *"Copy and paste of a selection of cues from one
project to another should be possible."* Two engine commands, drawn so that neither client has to know
what a cue looks like on disk. **`document.copy <ids>` is a read**: copies of the cues as one canonical
`<Fragment>`, written by the node writer show.xml is written by, and published at `document/clipboard` -
in the tree and not handed to a caller, because the document is the tick thread's and a client reads
the tree (§14.16, rule 2). **`document.paste <parent> <index> <fragment> [ids]` is the write**: the
fragment is read INTO NEW IDENTITIES before the show is touched - the builder that reads show.xml, fed
new names in a pre-pass - every attribute the schema says refers to a cue and whose value was one of the
copied cues is re-pointed to the new name, and the cues enter at the member position in one undo step.
The names drawn ride on the record, so a replay draws none: the same rule `cue.create`'s optional id
follows. A reference to a cue the fragment did not bring stays as written, which in the same show is the
cue it meant and in another is a target `validate()` names; a media cue's `file` is a name the other
bundle may not have, and the window says so as it does for any missing file. **Between two windows the
carrier is the operating system's clipboard**: the desktop mirrors the published fragment onto it when
it changes and pastes from it, so ctrl/⌘-C in one process and ctrl/⌘-V in another is exactly one copy
and one paste. The page reaches both too - copy of what is picked, paste of the published fragment
after the anchor - so the desktop is not their only route (rule 3); what the page cannot do is cross
a process, since it has no way onto the system clipboard that does not ask the person first.

**THE FOLD IS THE SHOW'S NOW, AND THE PRESET HAS A GESTURE (2026-09-18).** *"Fold state should be
recorded in project file."* §14.1 kept folds out of the document as one operator's screen; the author
wants a show to open as it was left, and the two are reconciled by WHERE it is kept: four state rows -
`group/folded`, `group/headerFolded`, `group/footerFolded`, `list/persistentFolded` - written by the
window when a fold is toggled and landing in state.xml beside the standby, so a fold never marks the
show unsaved and a locked show still takes it. The model's fold set is still what is drawn between
rebuilds, so a toggle shows at once; when the rows are rebuilt the set is seeded from the flags, which
is how the next opening finds them. *"Drag and drop with alt onto a group label adds this cue to the
header ... ctrl+upArrow and downArrow, since this way we can move the preload/preset up or down nested
groups."* Both are one write to `preset`, the mark §13.7 made the decision: **alt-drop** on a group the
cue is inside names that group and is refused for one it is not, since the engine would only warn that
no header will ever prepare it; **ctrl/⌘-up** steps the mark outward through the ancestors from none to
the innermost to the outermost, **ctrl/⌘-down** inward back to none, for every picked cue. Plain arrows
keep the pointer. Also this round: one foot row under GO instead of two mostly-empty ones, a third of a
row of ground between the panes, Cut on ctrl/⌘-X as a copy and then the deletes, and the title bar's
dash as the UTF-8 it is. *And the gesture's first use found the band it aims at was incomplete:* "the
headers are not updated when adding an element to them for preloading" - the desktop's header band
drew the header's own cues and never the ones a preset mark derives into it, which the engine has
published as `headerDerived` since the frames round and the page has drawn in italics since 5.13. The
model now appends them after the written lines, marked derived - not walked into, not in the index, so
the pointer and the pick land on the cue's own row - and the list draws them dimmed and italic with the
word *preset*.

**THE FOOTER AS A PLACE, AND THE LADDER (2026-09-18).** *"The footer items are moved to the footer
with shift+alt drag and drop on the group title, or drag and drop directly in the footer if it already
exists. Ctrl/⌘+ArrowDown puts the selected cue in the footer ... while ctrl/⌘ is not released the cue
will point to either a header or, if down past the deepest nested group, will end up in the footer."*
A footer is a container and not a mark, so this is `object.move` - and a move needs a name the tree
did not give: `group/header` and `group/footer` now publish the sections' identities as `list/persistent`
always has, and a band and every row inside a section carry it. **Shift+alt on a group title** moves the
cue into that group's footer, `group.role` making the footer first when there is none and the move
following on the pass that sees its name (the import's own shape). **A drop on a section's band** puts
the cue into that section at its end; **after a section row** puts it after that row inside the section.
**The ladder** is one gesture over the whole of where a group can hold a cue: ctrl/⌘-up walks outward
through the headers, ctrl/⌘-down inward to none and then one step further into the innermost group's
footer; from the footer, up is back among the members. Each step is one command and the foot says what
happened - "prepared in Scene's header", "into Scene's footer", "back among Scene's members" - which is
the arrow the author asked for, in words. *And the derived line is a thing that can be picked up:*
"dragging a preset line out of the header should remove it from the header; if it falls on a
different group top line or header, then move this preset." The line is the mark, so dragging it
moves the mark - onto a group the cue is inside, or that group's header band, and the cue is
prepared there instead; onto the group it already names, nothing; anywhere else, a member row, a
stranger group or no row at all, and the mark is cleared. The drag carries the cue's id behind a
`preset:` prefix so the drop knows it is moving the mark and not the cue, and every lookup of a cue by
id prefers the cue's own row to a derived line of it, since reasoning from the line's place would put
the cue in the wrong container.

**EDITING IN THE LIST (2026-09-18).** *"It would be nice to be able to edit the userID, name, prewait,
duration and postwait right in the cue list by double clicking. Once the data is typed, Enter or a click
outside the edited field validates and dismisses, or Esc to cancel the modification (in this case don't
panic). Arrows allow to navigate in neighbouring fields while validating any edits."* One box, moved
about, living on the list's own scrolling surface so it rides with the rows. A double-click on the
number, the name or one of the three times opens it over that cell - the cells carved exactly as the
painter carves them, so the box lands on the words; a column the cue may not write says so instead of
opening (a media cue's duration is its file's). Enter and the focus leaving commit, and only when the
text changed, since leaving a box as it was is not a decision; Esc cancels INSIDE the box, so the shell
never sees it and PANIC never hears it; the arrows commit and move - up and down through the cues in
the same column, skipping bands, derived lines and cues that lack the column; left and right along the
row - and Tab goes along the row too. A box open over a cue follows it through a rebuild and shuts when
the cue is gone; a locked show opens none. Every commit is the same `node.set` the inspector's field
sends. *Found in the first hour, by driving double-clicks from a script and tracing every click the list
received:* the first click of a double-click picked the row and OPENED THE INSPECTOR, which narrowed the
list and slid the right-carved columns from under a pointer that had not moved, so the second click read
as another cell or none - and in a default-sized window the name of an indented cue was twelve pixels
of column once the inspector was open. Two things followed. The panes' shares changed so the list keeps
a width it can be edited at; and, the author's own instruction - "don't open the inspector on a double
click" - the row is picked at once but the inspector opens only after the system's double-click time has
passed without a second click, and never when a box opened. A pick that is not a click - all, or a cue
just made - opens it at once.

**SCRUBBING THE RUNNING PANE (2026-09-18).** *"I'd like to be able to scrub active cues and groups.
Scrubbing within the bounds of the strip is 1:1 but dragging with the cursor going above or below
increases the precision of the increments for fine tuning. This is especially important on long media
files. Pushing against the window edge in precision mode will keep sliding the cursor in the given
direction. For groups this slides by the same amount all cues of the group. We'll see if we can
'resurrect' past cues this way."* One command, `run.seek` (§12.4), and one model, `model/Scrub`: a press
on a sounding media run's strip, or on a running scene's row, takes the head where it is - a grab, not a
jump to the pixel under the finger - and every pixel of travel moves it, 1:1 inside the strip, halving
every strip-height above or below it down to one part in 256, integrated step by step so a hand coming
back down for coarse travel does not make the head jump; a pointer against the WINDOW's edge keeps the
head sliding at ninety pixels' worth a second at the gearing its height sets, on the pane's own
twenty-five-hertz pass. The pane sends one record per position the hand settles on - at most five a
second, since each seek stops and re-asks a voice - and one when it lets go; a grab that never moved
sends nothing. The ghost head is drawn in the picked colour with its clock and gearing in a box beside
it, the engine's own head staying where the sound is until the seek lands; the cursor says which strips
scrub before any press. A scene is scrubbed by the same drag on its row, geared to the longest thing it
is playing, and the engine RE-SEATS it: the members are ended and built again under the same group run
from the solver's answer for the scene at that second, which is the load-to-time's own machinery
(§3.13) turned on one group - and the author's "resurrect past cues" is exactly what that gives, a member
already over coming back when the hand goes before it, at the cost of a member's run being a new one. A
manual sequence has no second to seek to and offers no drag. What the solver learned for it: a group
that IS a chain's origin is not itself timed, so the aim on the group reads its members against the
offset directly; every kind of member is placed, not media alone, since a fade due four seconds after
the instant has to be waiting there or the scene never fires it; and an inner scene's members follow
their group - none planned under one still due, all over under one that is over. Also found and fixed:
a jump into a scene inside a scene built both as top-level runs, because the plan's ancestor groups
carried no ancestors of their own. On the page a running cue's or scene's row gets a seek box instead
of a drag - the same record, typed - which is rule 3 kept. The author's second thought - that a scrub is
"scrubbing through the load to time history" - is the next round's, with load to time itself.

**LOAD TO TIME (2026-09-19).** *"Load to time is opened with an item of the Show menu. The Inspector
panel turns into a history vertical stack. The cue list items can be spaced vertically to show the
various intermediary steps recorded with the focus on the selected cue or group. Cue or group can be
changed and the load to time readjusts to it. The panel is only closed when the user triggers Go. A
numerical time value appears so the value can be set from the keyboard and a pointer shows when this is
in the history."* And the reframing that changed the engine under it: *"1 [scrubbing] is like scrubbing
through the load to time history … We might need to adapt the original design of the load to time
history."* The adaptation is §13.10's: the solver reads the steps when the aimed cue has one, and the
jump retimes them. The window: *Show → Load to time…* (ctrl/⌘-T) opens the aim on the picked cue, else
the standby, before it fired; the slot beside the list - one slot, three things that can stand in it -
takes the history panel in the inspector's place. The panel is the list's steps newest at the top, each
with how long ago and how it was fired, the aimed cue's own step lit, the steps past the instant dim
since a load would take them back, and the pointer drawn as a line among them where the instant falls;
a click on a step aims before it, as the page's chips do. Above the stack: the aimed cue's name, the
offset as a number that can be typed (Enter commits; `before` or nothing is -1) with a *before* button
and the *Load* button, and the engine's answer in words - read from what happened or from the list's
order, what would be sounding and how far in, what is due, where the pointer would land, what it could
not know. Under the aimed cue's own row in the list, the steps the list took after it fired are laid as
rows of a new kind, `step` - one per step at its offset into the cue, italic, with the aim's own pointer
row among them at its offset - so the intermediary steps are the spacing; a click on a step row re-aims
at that offset. A pick moves the aim to the picked cue at the same offset. Every change is one
`list.aim`; the button is one `list.loadToTime`; and only a GO - the button, Space, the list - takes the
panel down, the menu item being the other way out. `model/LoadToTime` reads the three nodes through the
engine's own JSON reader, which is the one place in this window that parses JSON, for the reason the
engine gives.

**THE UNDO HISTORY (2026-09-19).** *"We also need an undo/redo history. This is an item in the show
menu. This also opens in place of the Inspector and shows a diff overlay on the cues as the user drags a
pointer. This is only applied with an OK button or dismissed with a Cancel button. Both will also close
the undo/redo history panel."* Two nodes the engine did not have: `document/undoHistory`, what Undo
would unmake newest first, and `document/redoHistory`, what Redo would put back nearest first - the
manager's own two lists, read in the after-tick beside `undoName` and never pushed (§14.9) - so that
where the show stands is a NUMBER, the count of transactions applied, and standing elsewhere is that
many `undo` or `redo` records, each one the log already replays; no new command. *Show → Undo history…*
(ctrl/⌘-shift-U) takes the slot beside the list: the stack newest at the top - the transactions Redo
would put back, dim; the pointer, "standing here"; the ones Undo would unmake; the show as opened at the
bottom, with the row the panel opened at washed in the standby colour. A click or a drag over a row
moves the pointer at once and sends the undos or redos when the hand settles, since every row passed
would otherwise be a flurry the log would keep. The diff is the window's own reading: when the panel
opens it takes a picture of the rows - every column the list draws and where each cue stands, by id -
and every pass compares the rows now against it: a cue that reads differently is washed and marked Δ,
one that was not there +, and one that is gone is named in the panel, since a row that is not in the
list cannot be marked in it. OK keeps where the show stands and closes; Cancel stands back where the
panel opened and closes. A locked show offers neither, as it offers no undo.

**THE LIVE RECORDER, AND THE START CUE IT WRITES (2026-09-19).** *"I'd also like to have in the show
menu a 'Live recorder'. This will create a sequence/sequential group in a new 'Live recorder' playlist
that will record all the cue starts (and controller level changes once this is implemented in the next
phase). This can be used to store timings triggered once by hand and then automated."* And the
reframing that made it small: *"4 is like dumping the load to time history to a group for replay."* So
it is two commands and no machinery of its own. `record.start` turns the history's keeping on -
`ListState` keeps every step on every list, unbounded, from that tick - and `record.stop` writes what
was kept into a TAKE: a timeline group named *Take N* in a list named *Live recorder*, made the first
time and found by name after, holding one start cue per step at the second it was pressed. A timeline
and not a sequence, deliberately: a sequence advances on completion and could not hold the seconds
between two presses, which are the whole of what was recorded. Every identifier the take draws rides on
the applied arguments in the order it was drawn, as a jump's do, and a replay hands them back; a locked
show keeps recording and refuses the take. `document/recording` says whether it is on. **The start cue is
new** (§12.4's kinds table): element `Start`, one attribute, `target`, a cue reference; it is a memo that
presses a button - fired, its run is done the next tick, and its target is fired BY NAME, standby
untouched, as `cue.fire` fires it. The fire is `cue.fire`'s own record, submitted by the next tick's
hook and never from the handler, so a replay - which runs no hooks - takes the record the session
logged and fires nothing twice; the one-tick lag is the record's price. A target that is a manual group
is refused as `cue.fire` refuses it. Both clients offer the kind: the desktop's new-cue row and
inspector, the page's kinds and fields; the page gets *record* and *stop recording* beside undo and
redo, the desktop *Show → Start the live recorder* (ctrl/⌘-shift-R), which reads the node to say which
of the two it is.

### 14.17 What Phase 5 built, against what section 14 drew

*Written 2026-09-17, at `7b9c73b`, from the phase rather than from its commit messages — which is
the whole of §13.16's argument for writing a close-out while the week is still legible.*

**The draft held for the engine and gave way for the client, and the giving way is the more useful
half.** §14 was drawn before a line of Phase 5 code existed, which was PR 5.0's entire claim on the
schedule. Half A vindicated it: eight pull requests landed close enough to what was drawn that every
departure fitted as a dated correction inside the subsection that had drawn it — the `armMedia`
early return that would have replayed an empty `kind` (§14.5), the autosave decision moving from the
after hook to the before one because a submit from the after hook is drained a tick late (§14.8),
the undo seam that turned out not to be pre-cut at all, its `nullptr` a literal rather than a
threaded parameter (§14.9), the blind retry that exists because `replaceFileIn` swallows the
platform error (§14.10), the colour ramp whose hue is *not* monotonic in frequency, which cost the
sweep test its obvious assertion (§14.12), and the CSV → generator → binary order the fixtures
insist on (§14.13). A draft wrong in a dozen particulars, each corrected where a reader would look
for it rather than in an errata list at the end, is this method working rather than failing.

**Half B's drawing did not survive contact with the author, and no amount of care in the drawing
could have saved it.** §14.3 drew seven views, one per pull request, each *"sized to earn one round
of the author's feedback rather than to be finished"*. The sizing was right. The contents were not,
and the reason is recorded here because it will recur in every phase that ends in a surface: **what
the author asked for after looking was never the next view on the list.**

**The evidence is a file list, and it is not close.** Half B's plan named `model/layout.js`,
`views/header.js` and `views/curve.js`; none of the three exists. What the console grew instead —
`model/remember.js`, `gestures/drag.js`, `gestures/fields.js`, `gestures/table.js`, `views/view.js`,
`views/common.js`, `views/values.js`, `views/reconcile.js` — is eight modules, not one of which any
plan drew. The four numbered pull requests that did land as drawn (5.9's rehearsal blockers, 5.10's
module split, 5.16a's fade points, 5.18's tests) are precisely the four with no layout in them. The
moment a pull request's subject was *how something looks*, the number stopped predicting the work.

**So the numbering was abandoned rather than defended.** From `90c20bd` onward the console's commits
carry no PR number, because attaching one would have been a fiction — a round that moves the
inspector to the foot of the window is not 5.13, and calling it 5.13 would have made the plan look
met while the author was asking for something else entirely. Eleven commits, in the order the author
asked:

1. **`90c20bd` — bigger type, readable greys, time columns, marks.** The first thing asked for after
   looking at the finished page was not a missing view: it was that the type was too small, the
   greys too close together, that Didi should carry preWait, duration and postWait as *editable*
   columns, and that Gogo's *Armed* and *Playing* words should be the yellow and green marks alone.
   A 25 % type scale became `--type`, a single token every size is derived from.
2. **`a6f29f1` — the inspector folds away what the engine says back.** UIDs, hashes and the rest of
   the machine's own bookkeeping into a collapsible *details* section, because *"these are not
   really necessary for the user"*. §14.2's generic inspector survived intact: the fold is a rule
   over `ACCESS` and name, not a list of fields.
3. **`26881a7` — the inspector reads in the order somebody works.** preWait before duration before
   postWait; cue types and subtypes grouped by what they do rather than alphabetically. The
   alphabet was never a decision, only a default nobody had questioned.
4. **`6748dd4` — the inspector can sit at the foot.** The author raised QLab, where the inspector is
   at the bottom, and asked for *both* arrangements rather than a choice — so labels move onto the
   parameter row in the foot orientation, to keep the scrolling down.
5. **`3115b10` — and it can sit between the panes, only when something is picked.** Didi and Gogo
   shrink to make room; nothing picked, nothing shown.
6. **`d6f70fe` — the panes move rather than jump.** The author asked for the contraction to be
   animated rather than an instant redraw, and for the panels to be told apart by tone. This is the
   round that produced `--panel`, `--panel-in`, `--panel-inspect` and `--panel-high`.
7. **`e592ac0` — a section is a frame that shuts, and the page remembers what the reader shut.**
   Asked for in three parts: a header line should be able to reveal its cue; the header and footer
   sections need a clearer delimiter, *"something like a collapsible frame"*; and the open/closed
   state has to persist. The third part is `model/remember.js` and `localStorage`, which is where
   §14.1's line falls — fold state is the client's, and the engine is never told.
8. **`2a5b436` — a move's index is a member's position, and many cues can be chosen.** Below.
9. **`09747f7` — the pointer may stand on any cue of its list.** Below.
10. **`f2989a0` / `5d3a171` — a cue can be dragged where it goes.** Explicitly excluded by the Phase
    5 plan (*"the console's own comment declines it; ▲/▼ and `object.move` stay the gesture until
    the author asks"*) — and the author asked, which is exactly the condition that exclusion named.
    The drop says what it will cost before it happens: a drag that would break a `preset` or carry
    marks with it says so in words at the insertion line.

**Two defects surfaced that no green test could have shown, and both were found the same way.**
Before building drag-and-drop on `object.move`, the engine was driven by hand and watched. The
index it takes counted *every* child element — `<Header>`, `<Footer>`, `<Persistent>`, `<Trigger>`
included — while every client counts members, because members are all the tree publishes. In a
group whose children are `[Header, Media, Osc, Group, Footer]`, the inspector's ▼ on the Osc made it
trade places with the `<Header>` element and moved no member at all. The button had done nothing
since the day it was written, under 73 green tests, because every test asserted through the same
member-blind arithmetic the door used. The fix is one rule in one place, `doc::isSequenceChild` and
`doc::rawIndexForPosition` (`document/Sequence.h`), asked by both the publisher and the door so they
cannot drift, and verified by exhaustive simulation over every arrangement of up to five children.
Chasing it turned up the second: `remove`'s standby repair asked for *"the next sibling with an
id"*, which in a group with a footer answers with the `<Footer>` element — a standby the write door
then refuses, so the repair silently did nothing and left the list parked on the cue just deleted.
That is the frozen-GO failure `remove`'s own comment exists to prevent. **The lesson is cheap and
general: probe the running engine before building on it.** A test suite written against an
implementation shares its blind spots by construction; a hand on the gesture does not.

**A third defect came from a decision instead, and is the better story.** The author reported that
the arrows would not let them stand on a cue *inside* a group to start from there. The fix was one
removed guard in `cue::CueList::findOnPath`, and the test written to pin the author's decision then
failed on GO — because a GO from inside a machine-parented group started the whole scene rather than
the cue the pointer was on. `Runner::descentTo` now stops at the first non-manual group, and
`ShowWalk`'s `mayLandHere` is deliberately narrower than `cue::mayStandOn`: a jump must not leave
the pointer inside a scene that is about to fire it. A decision, written down as a test, found a bug
the decision was not about.

**Three questions were settled, and one of them had been open since Phase 2.**

- **X — the cursor.** The standby pointer may stand on any cue of its list, including inside a
  group, and GO fires the one it is on. PRD §3.5 and §3.6 were amended; §3.27's sampler sentence
  contradicts it and is **flagged, not amended** — it is the author's.
- **Y — media arrives through the desktop client**, because a browser is never told a dropped file's
  path. Not a scheduling call: a capability the first client cannot have (§14.16).
- **E — the desktop client runs in process**, settled on 2026-09-17 after four phases of being
  deferred. §14.16 is rewritten against the answer, and the rewrite made that subsection *smaller*:
  no HTTP poll, no WebSocket, no second copy of the tree, no blocking client thread, no rule set
  aside. Settling it before a line was compiled was the point — §14.16 had recorded that a client
  started without an answer would answer E by accident.

**The author's own reading of §3.2 was corrected, and the correction is theirs, not a concession.**
This draft had been quoting the slogan — *"nothing the UI can do that the API cannot"* — as though
it bound controls. Read whole, §3.2's binding word is **action**: *"Every gesture-reachable action
also exists as a named command… Modifiers and gestures are an accelerator layer over a complete
command set, never the only route."* The page has had sliders, number boxes, keyboard shortcuts and
now a drag-to-reorder since Phase 3, not one of which exists in the API and not one of which broke
anything, because each of them ends in a named command. The desktop client accordingly *"gets as
much slack as it needs"*; the tablet is a subset of controls, never of powers (§14.16).

**What the page is, at the close.** Rows keyed by id and reconciled in place, so a 500-cue show
renders in 6.5 ms where it took 560 (M24, and the trigger scan was 97 % of it). Nine ES modules over
four directories, no build step, edited while a show runs. Frames that shut and are remembered per
show. Multi-selection with anchor-and-range, bulk edit across the selection through the same generic
inspector. Drag-to-reorder with a refusal that speaks. The standby on any cue of its list. Undo,
save, the dirty dot, the lock, recovery. A running pane with round pills and prune. Timbre numbers
on a run row. 115 console cases under `node --test` plus 73 ctest tests plus two Phase 5 black-box
drivers, on three platforms in six CI jobs.

**What §14 drew for the page and nobody built — deliberately, and now the desktop's.** The display
presets and show mode (5.14), the fade-curve editor (5.16b) and the spectral bar (5.17), joined by
the one piece of the header pane the frames round did not absorb — *mark as preset*, which is an
inspector control. These are the four views the author assigned to the desktop only on 2026-09-17.
Three of them could not have been sketched on the page in any case; the fourth is a visibility rule
over panes that already exist. The header pane as a *pane* no longer exists as work at all: the
frames round built its written-and-derived lines inside the group's own header frame, which is
where they belong.

**What is still open at the close**, none of it blocking: the 8.6 MB full-tree poll against LISTEN
or a values-only read; the ramp's non-monotonic hue and §3.30's *(proposed)* idle-colour policy;
5.16a's two hazards (bus re-point, N-argument datagrams) and its proposed leave-from-the-run's-level
rule; the PRD amendment backlog, group D, plus §3.27's flagged sampler sentence; the Mac mini's
M22/M23/M24; the `WFG_SKIP_SMALL_BLOCKS` experiment; a narrowing conversion at `cue/Runner.cpp:1181`
that MSVC warns on; the ▲-then-▼ that does not round-trip out of a non-manual group; and
`siblingAfter`'s remaining hole, where a `<Trigger>` or a disabled cue is a member by the sequence
rule and refused as a standby — whose honest repair is for that walk to ask the cursor rather than
the children, a decision about the standby and not about the index.


## 15. Devices, ports and interfaces — what the show settings window needs

*Written 2026-09-22, after the author asked for "a network tab in the parameter panel, show
settings … several clients, enable Rx and Tx for each, with the IP, port and NIC", a target
menu on every OSC cue, and the same for MIDI. Drawn against `main` at `4ff724a`.*

### 15.1 The four decisions the author took

**D1 — a network device IS a mount, made editable.** Not a second object beside it. A mount
already holds a prefix, a host, a port, an OSCQuery port and a read-back mode; what it lacked was
a name, two switches and the ability to be written at all. Adding a `<Device>` element would have
meant two resolution paths in the runner and a cue having to say which kind it was aimed at.

**D2 — the cue's target menu rewrites the address prefix; no new cue attribute.** A network cue
carries the whole address it writes. Which device it is aimed at is therefore the front of that
address, and the menu reads it and writes it back. The alternative — a `target` row naming the
device — is a second truth that can disagree with the first, and the disagreement would be
invisible until a show night. What it costs: every device has one root its cues share, so a desk
with no single root (an X32: `/ch/…`, `/bus/…`) needs a prefix that is not really its own. The
author accepted that, and the prefix-stripping flag that would fix it is in PRD §6.9 as a
proposal.

**D3 — the machine-side bindings live in the show**, as the audio interface already does
(`audio/outputDevice` is `rw`, `persist=show`). The command-line flags keep overriding, so every
driver and every test is unchanged, and something this machine does not have is a warning and a
fallback rather than a refusal to open.

**D4 — strict senders now, per-device Rx stored but not processed.** The author's words: *"Have a
toggle to strictly parse who sends and if they are among the targets or not. Rx would be for
processing data sent by the device. This will come a bit later with the state machine processing
or for a device that can send itself commands to create the cues."*

**D5 — vanilla OSC first.** *"We might make specialised targets with extra features (tree with
values, OSCQuery enabled 2 way communication, device programming Go.dot directly...) But for now,
let's start with vanilla OSC."* So a device made from the window is an OPAQUE mount, and what
makes a device vanilla is simply the absence of a namespace file. No `kind` row is added until
there is a second kind to name.

### 15.1a One device, several roots

**The author's own desk decided this.** A DiGiCo S21 reached directly rather than through its
sidecar answers at three roots with nothing above them: `/channel/{n}/…` for the whole strip
(name, gain, trim, EQ, both dynamics sections, sends, pan, mute, solo, fader), `/console/…` for
ping, pong, resend and the channel counts, and `/digico/snapshots/fire` for snapshot recall. The
command set is in the S21-HiJack documentation folder, `DiGiCo S OSC Commandset_OSCpaths.csv` and
`…_channelNumbers.csv`. Through the sidecar it is one root, `/s21`; direct, it is three.

So `prefix` holds them all, **space-separated**, and `prefixesOf` splits it. A space is the
separator because an OSC address can never contain one, and a row with a single prefix reads
exactly as it always did, so every show written before this is unchanged and no attribute grew a
type. The alternatives were worse: three devices for one console means three rows, three names
and three `sent` counts; one invented root means the address in the cue is not the address in the
manual it was copied from.

**A described device keeps one root.** Its namespace file is one tree and mounts in one place, so
a second root would route messages to a box whose nodes are published elsewhere, and every write
under it would come back `bad-address`. Refused at load, with a sentence.

**`prefixMatchLength` is the one rule, and the client calls it.** `wfg_client_model` links
`wfg::engine`, so `model::deviceOf` asks the engine's own function rather than restating it. The
first version restated it and they disagreed: the client took the longest match while
`MountTable::mountOf` returned the first device whose prefix fitted, walking a map keyed by
identifier. With `/desk` and `/desk/aux` both declared, which one got a cue depended on the
alphabetical order of two random eight-character strings, so the panel could name one device
while the message went to another. The engine takes the longest match now, and both ask the same
function.

**`retarget` is identity on a cue's own device**, which with one root was too obvious to write
down and with several is load-bearing: a cue on `/digico/snapshots/fire` re-aimed at its own S21
would otherwise come back `/channel/snapshots/fire`, because the rewrite lands on the first root
and the cue was on the third. Without it the menu would never look selected, and re-picking the
device already shown would silently move the cue to another vocabulary. Found by a test, not by
reading.

### 15.2 An opaque device, and what it costs

A mount with no `namespace` used to be refused at load. It is now DECLARED: `MountTable::declare`
puts an entry with no nodes in the table, `MountDeclaration::opaque()` is the test, and
`MountTable::write` passes an unknown address through when — and only when — the mount that covers
it is opaque. Under a DESCRIBED device the same address is still `bad-address`, which is the
whole value of describing one: the show said what that box has, this is not among it, and saying
so now beats a datagram that leaves and is ignored.

What an opaque device gives up, and each is a refusal rather than a silence:

- **nothing is published under its prefix**, so no client can browse it;
- **no coercion** — the atom the cue spells is what goes out, which is why a cue carries its own
  type;
- **nothing is stored** — a value nobody can read back is not a fact about the device, only about
  what was sent, so `mount/sent` is what a rehearsal reads;
- **it can never be asked**, so `wait="verified"` against one is refused when the show is read.
  `canBeAsked()` and its read-time twin in `ShowDocument::validate` both test it.

**`/godot` joined the reserved prefixes in the same round.** Nothing had refused a mount there.
It had been harmless only because a prefix could only be hand-written by somebody who knew what
it was; the settings window is a box a person types one into, and a device mounted at `/godot`
would shadow every address the engine and its clients use.

### 15.3 What carries an edit to the socket

`refreshMountDeclarations (document, mounts, bundleFolder)`, called from the after-tick inside the
`showRevision` block — beside the analyser re-offer, and for the same reason: a GO writes the
standby, which is a state row and moves no revision, so the GO path pays nothing. A show edit
pays a dozen attribute reads per declared device, in memory.

It distinguishes three kinds of change, and the distinction is the point:

- a device the table has never seen is **loaded** (or declared, with no namespace file);
- a changed **prefix or namespace file** is a reload, because both decide what is mounted;
- **anything else** — host, port, query port, read-back, rate cap, name, rx, tx — replaces the
  declaration and KEEPS THE NODES. Re-reading a namespace because somebody retyped a host would
  throw away every value the tree holds for that device and every read-back in flight, to arrive
  at the same list of nodes. During a tech rehearsal, that is the whole session;
- a device the document no longer declares is **unloaded**, or its prefix would go on claiming
  addresses for the rest of the session.

### 15.4 Tx off, and why it is a warning

`writeOscNow` marks the job `notSent` and returns before it queues, and before it sets up a
verify — a `verified` cue against a device nobody is talking to would otherwise wait out its
whole timeout and fail, which is a failure produced by a setting rather than by anything in the
rig. `advanceSends` then ends the run as `done` carrying `runWarning::notSent`.

A warning and not an error, deliberately. The request was legal and the cue did everything it was
asked to; the one thing it did not do is the thing somebody switched off five minutes ago.
Reporting it as a failure would fill a running pane with red and teach an operator to stop reading
the colour that means something is actually wrong (§4.8).

### 15.5 Strict senders

`osc::SenderGate` is the `TriggerIndex` shape: the tick thread builds an immutable `Allowed`
(the flag, plus the hosts of every declared device with `rx` set) and publishes a `shared_ptr`;
the UDP handler takes one reference under a short mutex and reads it. The set a datagram is judged
against may be one tick old, which is correct — it was the rule when the datagram arrived.

The check happens **before the bytes are decoded**, because what it refuses is a SENDER and not a
message: decoding first would spend the work and, worse, would make a malformed packet from a
stranger report the wrong reason. A refusal is a `Drop` record carrying `reason::unlistedSender`
and the sender's origin, and `/godot/network/refused` counts them — the whole failure mode of a
filter like this is a surface that does nothing with no way to find out why.

**It gates the OSC port and not the WebSocket.** A client is a client and a device is a device;
gating clients would lock an operator out of the very setting that locked them out. The black-box
driver turns the filter off through that door on purpose, which is also the proof that it exists.

**Matched on the address alone**, not the port: a device's source port is whatever the operating
system gave it and is not what anybody typed into the show. Two boxes behind one NAT share an
entry, which is a real limitation of the setting and is why the default is off.

### 15.6 The document gains a container

`<Show><Network/></Show>`, owner `network`, carrying `strictSenders` today and the interfaces
next. A container rather than an attribute on `<Show>` because an interface will have an
identifier, a name and two port numbers, and an attribute cannot grow children.

`ShowDocument::ensureContainers` is new and is called by the constructor AND by `adopt`. `adopt`
replaces the root, so a show read off a disk had exactly the containers its file happened to
carry — which is also why `createMount` could not give a device to a hand-written `show.xml` with
no `<Mounts/>`. That was latent long before this round; it is fixed here rather than in each
create, because the next container would forget it too. Every fixture gained one empty
`<Network/>` line, and so will every existing show the first time it is saved.

### 15.7 The client

`model/Devices.{h,cpp}` is std-only and pure: `readDevices` (one `all()` pass gathered by
identifier, the `readOutputs` shape — `childrenOf` is banned), `deviceOf` (longest prefix, ending
on a separator, restating `MountTable::mountOf` so the menu and the engine cannot disagree),
`retarget` and `targetChoices`.

**The target line is derived and is not a row.** `Control::deviceRef`, named `device` and labelled
`target`, inserted by `aimAtADevice` beside the media pass that builds the direct-out menu. Its
`address` is the cue's `address` row and its choices are WHOLE ADDRESSES — so picking one commits
through the `node.set` the panel already has, and the current device is found by matching the
choice key against the current value. No new command, no new attribute, no new refusal.

It is named `device` and not `target` because `target` is already a cueRef row on three other
kinds and `shapeOf` keys on the row name. It is **not offered over a multi-selection**: every
other row writes one value to N addresses, and this one would write a rewrite of each cue's own
address. Aiming several cues at a device is worth having and is a command that does not exist.

`AudioSettingsWindow` became `ShowSettingsWindow`; the menu item is "Show settings...", the first
tab is "Audio" rather than "Interface", and `NetworkPage` is the fifth. Its words are WFS-DIY's
Network tab's words — Name, IPv4 Address, Tx Port, Rx, Tx, ADD, and the `OSC Filter: Accept All` /
`Registered Only` button — because it is the same person reading them. What it adds: a Prefix
column (a Go.dot cue carries its device's root), no six-row ceiling, and no Protocol column until
there is a second kind. **Nothing on that tab is applied**: a device has no hardware to reopen, so
every cell is a `node.set` and the after-tick re-read is what makes it real.

### 15.8 What M-B and M-C still owe

**M-B, MIDI.** `port,outputDevice` / `inputDevice` / `rx` / `tx` / `bound` / `problem`, an owner
`ports` for `/godot/port/inputs|outputs`, binding from the document at open with the CLI flags
overriding, `port.create`, `midi.rescan`, a MIDI tab of the same shape, and `portRef` menus on
`midi,port` and a MIDI trigger's `port`.

**THE AUTHOR'S DECISION OF 2026-09-22, and it settles two things.** The question that produced it
was theirs: *"What is best if switching USB ports for instance?"*

*A cue and a trigger name the DECLARED PORT, strictly.* Today a MIDI trigger's `port` is matched
against the JUCE device name — `MidiInputs.cpp` stamps `source->getName()` — so the parameter
table's "the declared port" has never been what the code does. The fix is to make the code match
the table rather than the other way round, and the USB question is what decides it: the show says
"Lights", the machine says which cable that is, and moving the interface changes ONE binding while
every cue and trigger that named the port goes on working. Matching a device name in a trigger
would mean editing every trigger in the show for a moved cable. An earlier proposal to accept
either spelling is withdrawn; there is nothing to carry (the Stop/Transport rename made the same
call for the same reason).

*A port stores BOTH the device's name and its identifier, and matches on the best it can get.*
`juce::MidiDeviceInfo::identifier` is OS-formatted and is not documented as stable: on Windows it
encodes the device instance path, so moving a cable to another socket usually changes it. A name
survives the move and is ambiguous between two identical interfaces. So: **identifier first, name
as fallback** — which is WFS-DIY's own rule (`Source/AppSettings.h`, whose comment gives exactly
this reasoning and cites JUCE's `openLastRequestedMidiDevices`). Where neither matches, or where
the name fits two devices, the port stays UNBOUND and says so: a MIDI cue arriving at the wrong
desk is worse than one that does not arrive.

*And the collision it uncovered.* `cue,number` and `midi,number` were two rows of one name on one
element. A cue's attributes are all published at `/godot/cue/<id>/<name>` with no owner in the
address, and the tree emits one node PER ROW, so a MIDI cue carried `/godot/cue/<id>/number`
TWICE — one string, one integer, both reading the single `number="12"` attribute the grammar
allows. The panel drew the row twice, which is how the author found it; the sharper half is that
a write reached whichever the schema lookup returned, so renumbering a MIDI cue changed its
program. Renamed to **`midi,data1` and `midi,data2`**, the MIDI specification's own words for the
two payload bytes, which are accurate for every type while the panel shows the word that fits.
`tests/DocumentTests.cpp` now refuses any element whose owners declare one name twice, and counts
the rows it examined so it cannot pass by looking at nothing.

*Where each half lives.* The NAME is `persist=show`: it is what somebody decided and it reads
sensibly at another venue, which is decision D3's rule and what `audio/outputDevice` already does.
The IDENTIFIER is `persist=state`: it is what this machine matched, not a decision, so it belongs
beside the standby and the folds — it never dirties the show, and a stale one at another venue
simply fails to match and falls through to the name. When a port binds by NAME, the engine writes
the identifier it found back to the state row, so the next start is exact; a state row is
writable under the edit lock, which is what lets that happen during a locked show.

**M-C, interfaces — THE RECEIVE SIDE ONLY (2026-09-22).** The per-device interface was dropped
before it was built, and `MountDeclaration::interfaceId` removed with it. Which card an outgoing
message leaves by is the operating system's answer: it reads the destination and picks the route.
WFS-DIY has had an interface menu for years that is stored in its project file and fed to no
socket at all, and the one socket in that program which does bind an interface is its PSN
receiver, because multicast has no routing-table answer. Where the choice is real is receiving:
bind to everything and hear every network, bind to one address and hear one. So:
`<Network><Interface/></Network>` with `address`, `oscPort`, `queryPort`,
`enabled` and their readouts; N `UdpEndpoint`s and N `OscQueryServer`s (the
fork takes a bind address — `SimpleWebSocketServer::start(port, suffix, localAddress, reuse)` —
and JUCE 8's `DatagramSocket::bindToPort(port, localAddress)` exists); `MountSender` choosing a
socket per device; and `network.apply` in `audio.setup`'s shape, because rebinding drops every
WebSocket subscription and must not happen under a GO.


## 16. Phase 6 — surfaces, strips, DCAs, sampler groups: what the tree, the commands and the log gain

Written on 2026-09-23, before the code, as §11 to §14 were: the approved Phase 6 plan drawn as a
text the pull requests 6.1–6.8 can be reviewed against rather than against memory. It is drawn
against `main` at `5ec980a`, the MIDI ports milestone — M-B of §15.8, which is where this phase
starts from — and PR 6.2 landed as `f43d104` while it was being written; §16.2 says where that
commit took a different line. Rows reach `docs/parameters/godot-parameters.csv` with the PR that
implements each of them, never before. Where this section and the code come to disagree, §16.12 at
close-out says which won.

The request was the author's, on the evening of 2026-09-22: *"Can you work on the DAW controllers
now, phase 6? Plan for standard Mackie protocol and then add some niceties for the Asparion D700.
Eventually we'll add others like the Icon V1 and P1, not in the scope here. Look into the show
settings to add a control window for this. There will be also velocity/pressure sensitive pads too
to trigger samples. Add the sample groups."* Every sentence of it lands somewhere below. Mackie is
the bridge of §16.6 and the D700's niceties are a layer over it; the Icon units become words in a
table when they arrive (§16.10); the control window is a tab and a panel (§16.7); the pads are
strips that are pressed (§16.5); and the sample groups are the scheduling mode PRD §3.27 has
described since 2026-09-07.

**What the phase is, before any of its names.** Until now everything that started a sound was a GO,
a trigger, or one cue firing another. Phase 6 gives the show hands: a fader somebody lifts to start
a sound and pulls down to end it, a pad hit harder to play louder, one fader that brings five cues
in two different scenes down together. Three things are needed for that, and all three are declared
in the show. A **surface** is the box on the desk. A **strip** is one fader or pad on it, and says
what it is for. A **DCA** is a named trim — an amount added to a level without touching the level
itself — that cues and groups are marked with. What the hands are doing tonight, where a fader sits
and which sound is under it, is published beside what was declared and never stored (§4.10); and
every movement is a named command in the log, so a replay of the night moves the same faders the
same way.

Four decisions the author took with the plan shape it — **Z**, **AA**, **AB** and **AC** in §9,
after Y — and §16.1 says what each means for a show:

| | decision | what it shapes |
|---|---|---|
| **Z** | one voice per armed member; a member that finds no free track waits, and says so | §16.5's arming, the word `voice` in `run/pending`, and `run.arm` |
| **AA** | velocity sets the level a clip starts at and pressure rides it while the pad is held — per clip, both off by default | four `media` rows, `levelForByte`, and two PRD sentences amended |
| **AB** | a Surfaces tab in Show settings, and a virtual surface panel in the desktop client | §16.7, and a sampler group that can be played with no hardware at all |
| **AC** | the engine and the virtual panel first, the generic Mackie bridge second, the D700 layer third | the order of every pull request in the phase |

Eighteen further decisions were taken with the plan rather than by the author. They are numbered in
§16.11 and cited in place as *plan decision N*, each an implementer's call written down so that it
can be overruled early rather than late — §14's convention, kept. Where this phase builds something
the PRD still marks *(proposed)*, it says so where it builds it, and PRD §6.9 lists every such item
under 2026-09-23 as a default the author may overturn.

**Where it starts, in the code rather than in the plan.** M-B landed at `5ec980a`: MIDI ports are
document objects, bound by identifier and then by name, with a MIDI tab and `port.create`. It leaves
two things this phase needs and pays for in PR 6.6 — a port's `rx` and `tx` rows are read by no
engine code, and nothing rebinds a port after start (`midi.rescan` is named in a row's description
and registered nowhere). Triggers fire from JUCE's MIDI callback thread through `Engine::submit`
with the velocity thrown away, with no press paired to its release and nothing continuous, and no
test can inject a MIDI message. The touch table PR 1.9 built for this phase — *"no surface exists
yet to disagree with it"*, says `tree/Touches.h` — is a local of each Console verb, where the Runner
cannot see it. A run's level is its own plus every ancestor's (`Runner::applyLevels`), only a fade
writes the first of those, and nothing called a DCA exists.

Phase 3's rule does more work here than anywhere since it was written — *the hook decides, the
handler applies, and a handler never submits* (§12.1) — because every edge a fader crosses is a
decision a replay has to reproduce without running the hook that made it. The tick stays at 50 Hz;
the audio thread is untouched, since levels reach it through `setLevelDb` exactly as today; and the
engine still links no client (`Console.h`'s `ClientHost` is the whole contract).

### 16.1 The four decisions the author took (2026-09-23)

Asked directly, one question each with a recommendation beside it; all four recommendations were
taken.

**Z — one voice per armed member.** Arming a sampler group arms every member on a track of its own,
from the GO, so a bank of eight pads holds eight voices while it waits for a hand. When there is no
free track the member does not fail: it shows *pending* in words and lands the moment a track frees
— §3.9e's waiting claim, applied to a voice for the first time. Polyphony is bounded by
`Show/Audio/@tracks` and by nothing else, and a designer who wants a bigger bank declares more
tracks, which a show already has to say (decision G).

The other shape was a group that declares how many voices it has and shares them among its members,
and §3.25 and §3.27 had leant towards it since 2026-09-07, on the argument that *"a full bank as one
track per cell is the wrong price"*. Two facts are against it. A strip's fader rides one run's
level, and a run's level reaches the audio as its TRACK's (`Runner::applyLevels` ends in `setLevelDb
(track, …)`), so two members on one track would share a fader: the one over the gunshot would move
the rain as well. And a voice handed out at the press is a file made ready at the press, where §3.9a
says fader-start is only possible because *"by the time the finger moves, the file is open, the
transport armed"*. One voice per member keeps a press what GO already is on an armed cue — a launch,
and nothing else. The price is paid in tracks, and the track set is fixed at load and exists whether
anybody holds it or not (§3.25), so what an armed member costs is availability; the waiting claim is
what makes a shortage of it visible.

For a show: a bank of twelve armed with `tracks="16"` while an eight-cue scene is still sounding
arms eight members and shows four `pending voice`, each landing as a cue of the scene ends. Z
answers §3.25's *(proposed)* claim shape — *"the author's to pick before Phase 6"* — and §13.15's
open question, and §3.27's **Voices.** paragraph is answered in place.

**AA — velocity sets the level a clip starts at; pressure rides it while the pad is held.** Per
clip, and both off by default. Velocity maps a press onto the level the clip starts at: a velocity
of one starts at a floor (`velocityFloor`, −40 dB unless the designer says otherwise), 127 at 0 dB,
and between them it is a straight line in dB. Pressure — polyphonic aftertouch or channel pressure,
whichever the pad sends — moves the same trim a fader would move, on the same scale, and only while
the pad is down. The author's sentence for it is the design: *"a pad is a fader without a motor."*
Off, every press starts at unity, which is what a fader strip gives anyway, so a member plays the
same from either kind of strip until somebody decides otherwise.

This amends a sentence rather than filling a gap, and the sentence had a reason. §3.16's gate row
and §3.27 both say *"No pressure, no XY"*, and both were written on 2026-09-07 against WFS-DIY's
Sampler, which is MPE-shaped — pitch, pressure and a position for every note — and which the author
chose not to reproduce here. What the amendment keeps out is exactly that: no XY, no per-note pitch,
nothing that turns a pad into an instrument. What it lets in is one thing, and not a new one:
pressure writes the node a fader writes (`/godot/run/<id>/trim`, §16.4) through the one function
velocity uses (`levelForByte`, §16.5). No new node, no mapping language, no second path to the
audio. WFS-DIY's `PressureMapping {enabled, direction, curve}` is the precedent for the shape and
nothing is lifted from it — it is JUCE 9, MPE-shaped and monophonic per channel — and its default is
not taken either: there the level mapping is on, here both are off.

**AB — a Surfaces tab in Show settings, and a virtual surface panel in the desktop client.** The
author asked to *"look into the show settings to add a control window for this"*, and the answer is
two things because it is two jobs. The **tab** is where a show declares its surfaces: each one's
profile, the port it is reached through for each bank of eight, the preset the hardware has to be
set to, what each strip is for, and the DCAs. It sits beside Audio, Network and MIDI because it is
the same kind of thing — a fact about the rig written into the show (§15.1, D3). The **panel** is
where a show is played without hardware: every strip of every surface drawn with a fader, a pad, a
name, a state word and a colour, driven by the mouse through exactly the commands a surface sends.
It is PRD §3.17's redundancy path — *"losing the D700 is a downgrade in feel, not capability"* —
built in the phase that brings the D700 rather than left for the tablet, and it is what makes AC
possible.

**AC — the engine and the virtual panel first, the generic Mackie bridge second, the D700 layer
third.** Sampler groups and DCAs are judged on a screen with no hardware at all, and driven in CI
the same way; real faders come second, through a Mackie Control bridge any MCU unit answers; colour,
the three-row display, the rings and the second bank come third, as a layer over that bridge. It is
the vendor's advice — *"for own development, use MIDI … Mackie-based with vendor extensions"*
(§3.16) — made into a schedule, and it is how the author works made into one too: the things that
have to be judged by looking, how a bank arms and what a strip says while it waits, are on a screen
before anything is plugged in. The codec (PR 6.1) is pure and needs nothing else, so it runs beside
all of it. HUI is not in the phase, which answers PRD §6.10's *"Mackie vs HUI first"* for now.

| decision | the PRD sentence it answers or amends | written where, 2026-09-23 |
|---|---|---|
| **Z** | §3.25's *(proposed)* claim shape; §3.27's **Voices.** | both in place, and §6.9 |
| **AA** | §3.16's gate row and §3.27's *"No pressure, no XY"* | both in place, and §6.9 |
| **AB** | §3.17's redundancy path, and the author's *"control window"* | §6.9 |
| **AC** | §6.10's *"Mackie vs HUI first"* | §6.9 |

### 16.2 The objects and their rows

**A surface is a box the show talks to through declared MIDI ports.** Faders, encoders, buttons,
displays: whatever the hardware has, its profile says (§3.16: a device profile is *"topology +
protocol"*). The show says *"the D700, on ports P1 and P2, expecting the Mackie preset"*; the
machine says which cables P1 and P2 are, which the ports already do (§15.8). Element
`<Show><Surfaces><Surface>`, published at `/godot/surface/<id>`, with one of four profile words:

- `virtual` — the desktop client's own panel, which needs no port;
- `mcu` — a generic Mackie Control unit, eight fader strips on one port;
- `d700` — the Asparion D700: Mackie plus its native display, colour and rings, sixteen strips on
  two ports;
- `midiPads` — a pad controller sending notes with velocity and pressure.

**A strip is one fader or pad on a surface, and it is the fourth kind of slot** — one of a fixed
number of places a running cue can hold, wait for, or be pushed out of (§3.9e). §13.2 reserved its
row and left the columns for this phase. What a strip is for is the layout's decision and not the
hardware's (§3.9a): a **dca strip** is pinned to one DCA and never reassigned; a **sampler strip**
is filled by whichever sampler group is armed, member by member, left to right. Element `<Strip>`
under `<Surface>`, published at `/godot/slot/<id>` beside the processor inputs and rack channels —
the precedent is a rack `Channel`, an element in one place and a slot in the tree — with `kind =
strip` and a derived `target`: the one node the strip's fader or pad is writing now.

**A DCA is a named trim that cues and groups are assigned to, and DCAs nest** (§3.28). Element
`<Show><Dcas><Dca>`, published at `/godot/dca/<id>`. The assignment is a mark on the member — `dca`
on a media cue or a group — and never a list on the DCA, so nothing flows downward (§4.12) and
assigning eight cues is a multi-select edit. Its `trim` is live: it rests at 0 dB, a show opens
there, and it is never stored.

**Two containers beside `<Network>`, not one somewhere else** (plan decision 1). A DCA is not an
audio object — §3.28 lets a video cue be assigned to one, trimming its opacity — so it does not
belong under `<Audio>`; and a surface is not a port — a virtual one has none and a D700 has two — so
it does not belong under `<MidiPorts>`. `ensureContainers` grows to `Lists, Mounts, MidiPorts,
Network, Surfaces, Dcas`, and every fixture gains two empty lines after `<Network/>`: the `c973bfa`
pattern §15.6 describes, and the cost to overrule early if it is the wrong one.

**A strip's index and a surface's strip count are derived, never written** (plan decision 2). A
strip is an object and objects are identity-addressed (§1), so where it sits is the element's
position, as a range's `index` is (§12.9), and moving it is `object.move` rather than a number that
could collide with another strip's. A writable count would make *how many strips* and *which strips
exist* two truths that could disagree. `surface.create` makes the strips the profile implies, and
`strip.create` adds one. *(Dated note, 2026-09-23: PR 6.2 was committed as `f43d104` while this
section was being written, and on two counts it took a different line from the plan this section
draws. The plan refused `strip.create` on the Mackie profiles, eight to a port being the hardware's
number, and made eight strips for a pad surface; the code lets `strip.create` add to any surface — a
Mackie unit with its extender is one surface of sixteen — and makes sixteen for a pad surface. The
tables below follow the code; §16.12 records which reading stands.)*

**A surface's `profile` is fixed at creation, and the row has to be able to say so.** The plan draws
it `rw` and PR 6.2 kept it so — `trigger/kind`'s shape, writable although §12.8 calls a trigger's
kind fixed at creation — and nothing yet refuses a profile rewritten afterwards, which would leave
behind the strips the old one made. `bus/width` is the shape that makes the sentence true: read-only
at the door, `persist=show`, written only by its own command *(proposed)*. The Surfaces tab shows
the word as text either way.

**`ports` is the first `refers` row that holds a list.** The check that warns about a dangling
reference — the `References` walk in `ShowDocument`'s warnings — looks the whole value up with one
`findById`, so a two-bank D700 naming `P1 P2` would be reported as naming a port that does not
exist. PR 6.2 splits the value on spaces there — an identifier never holds a space, so nothing a
single-valued row could say is lost — and checks a surface's ports one by one. `mount/prefix` is
space-separated too and never met this, because it refers to nothing.

The rows, grouped by where they are published. Every one carries the panic policy `park`, and its
resting value is its default.

| Node | Type, default | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/surface/order` | `s` | ro | none | the surfaces declared, in document order — the order a sampler group fills sampler strips in, and then each strip's `index` |
| `/godot/surface/<id>/name` | `s` | rw | show | what the show calls it: *"The D700"*, *"Pads by the desk"* |
| `/godot/surface/<id>/profile` | `s`, `virtual` — `virtual \| mcu \| d700 \| midiPads` | rw | show | what kind of surface it is, which decides how its strips are driven and what each has; fixed at creation, and the strips it implies are made with it |
| `/godot/surface/<id>/ports` | `s`, refers to `port` | rw | show | the declared ports it is reached through, space-separated, in bank order: the first carries strips one to eight, the second nine to sixteen. The bank is the port and never the message (`docs/D700_CONTROL_GUIDE.md` §1.1). Empty on a virtual surface |
| `/godot/surface/<id>/preset` | `s` | rw | show | the preset the hardware must be set to — *Mackie* for the D700 — written down and shown to the operator, never detected: two presets can differ by one button, and a surface cannot be fingerprinted from its traffic (`docs/godot-asparion-d700-protocol-0.1.md` §5) |
| `/godot/surface/<id>/enabled` | `T`, true | rw | show | off, nothing is sent to it and nothing it sends is heard; its strips still exist and are still filled, so a surface left in the van is still part of the layout |
| `/godot/surface/<id>/channel` | `i`, 0 (0..16) | rw | show | `midiPads`: the channel the pads send on, nought for any |
| `/godot/surface/<id>/firstNote` | `i`, 36 (0..127) | rw | show | `midiPads`: the first pad's note; the strips follow it upwards, one note each |
| `/godot/surface/<id>/strips` | `i`, 0 | ro | none | how many strips it has, counted off its `Strip` children |
| `/godot/surface/<id>/connected` | `T`, false | ro | none | the engine is talking to it tonight: every port it names is bound, with `rx` and `tx` on. A virtual surface always is |
| `/godot/surface/<id>/problem` | `s` | ro | none | why it is not, in one sentence — a port with no device behind it, a port whose `rx` or `tx` is off, a profile that names no port |
| `/godot/surface/<id>/serial` | `s` | ro | none | the serial the hardware reported in the Mackie handshake, when it made one. Nothing depends on it; it is the one identifier that survives the operating system renumbering the ports |
| `/godot/surface/aim` | `s` | ro | none | *(2026-09-25)* the media cue every surface's rotaries edit on their EQ and Send pages, set by `surface.aim` — a SELECT on a sample strip, or a click on a running cue's name in the window. Not the window's pick and not the list GO acts on; empty when the cue it named is gone |
| `/godot/surface/<id>/page` | `s`, `show` — `show \| eq \| send \| fx` | ro | none | *(2026-09-25; `fx` 2026-09-26, §17.16)* what this surface's rotaries show. The surface's own, moved by its EQ, Send, FX and `*` buttons and by no command — which page a controller shows is where its hands are, like the window's selection. Published from the runtime half, every tick |
| `/godot/surface/<id>/pageIndex`, `…/pageCount` | `i`, 0 / 1 | ro | none | *(2026-09-25)* which page of that kind, from nought, and how many there are on this surface: the EQ's sixteen controls are one page on sixteen rotaries and two on eight |
| `/godot/surface/<id>/edited` | `s` | ro | none | *(2026-09-25)* the address the page last wrote, empty since it came up — how the window knows the surface is adjusting a cue, and which control it moved |
| `/godot/slot/<id>/kind` | `s` — gains `strip` | ro | none | derived from the element, as for the other two kinds |
| `/godot/slot/<id>/surface` | `s` | ro | none | the surface the strip belongs to, from where it sits |
| `/godot/slot/<id>/index` | `i`, 0 | ro | none | where it sits on its surface, from nought: fader one is index nought |
| `/godot/slot/<id>/role` | `s`, `sampler` — `sampler \| dca` | rw | show | what it is for (§3.9a, §3.27); the two may be mixed on one surface |
| `/godot/slot/<id>/dca` | `s`, refers to `dca` | rw | show | the DCA a dca strip rides; read only when the role is `dca`, and naming nothing says *unassigned* and moves nothing |
| `/godot/slot/<id>/endpoint` | `s`, `absolute` — `absolute \| gate` | ro | none | what the hand control is (§3.16): a fader, whose position is the value, or a pad, a press and a release with a velocity at the press. From the profile — `midiPads` strips are gates, every other profile's are faders — and published so a client draws the right thing and the engine knows where a fresh run's trim starts |
| `/godot/slot/<id>/target` | `s` | ro | none | the node the strip is riding now: `/godot/dca/<id>/trim` on a dca strip, the holding run's `/godot/run/<id>/trim` on a sampler strip, empty with nothing on it. It changes at every handover, which is how one fader rides a different sound after the bank changes |
| `/godot/slot/<id>/word` | `s`, `free` — `free \| dca \| unassigned \| armed \| pending \| playing \| held \| stopping \| closing` | ro | none | what the strip is doing, in a word for the display and never colour alone (§4.8); §16.5 says when each applies |
| `/godot/slot/<id>/cue` | `s` | ro | none | the member cue on the strip — the holder's, or the first waiter's — so a display can show a name and a number |
| `/godot/dca/order` | `s` | ro | none | the DCAs declared, in document order |
| `/godot/dca/<id>/name` | `s` | rw | show | *"Band"*, *"Ambiences"*, *"Everything"* — what a client and a wide display show |
| `/godot/dca/<id>/shortName` | `s` | rw | show | the name for a seven-character scribble strip, written by somebody rather than cut by the machine (§3.16) |
| `/godot/dca/<id>/dca` | `s`, refers to `dca` | rw | show | the DCA this one sits inside, or empty at the top. *"Everything"* over *"Band"* over one guitar is three terms in one sum; a cycle is refused when it is written and when the show loads |
| `/godot/dca/<id>/trim` | `d`, 0 (−120..12 dB) | rw | none | the trim this DCA applies now, added to every run whose cue is marked with it and to every run under a group so marked. Never stored (§4.10), which is also why writing it is not undoable |
| `/godot/cue/<id>/shortName` | `s` | rw | show | any cue's name for a seven-character display, authored (§3.16) |
| `/godot/cue/<id>/dca` — media, group | `s`, refers to `dca` | rw | show | the DCA this cue or group is assigned to; adds that DCA's trim, and its parents', to every run of it — level only, one to one in dB |
| `/godot/cue/<id>/dca` — fade | `s`, refers to `dca` | rw | show | the DCA whose trim this fade moves instead of a cue's level; when it is set, `target` is not read and `stopWhenDone` means nothing |
| `/godot/cue/<id>/takeover` — group | `s`, `group` — `group \| strip` | rw | show | sampler: what arming this group does to the sampler groups already armed (§16.5) |
| `/godot/cue/<id>/release` — media | `s`, `playOut` — `hold \| playOut` | rw | show | sampler member: what letting go does. `hold` stops the clip after a short fade; `playOut` lets it run to its end, and silence on a fader is a mute |
| `/godot/cue/<id>/secondPress` — media | `s`, `restart` — `restart \| noop \| stop` | rw | show | sampler member, play-out only: what a press does while the clip is playing — from the top, nothing, or the release fade |
| `/godot/cue/<id>/velocity` — media | `T`, false | rw | show | sampler member: whether a press's velocity sets the level the clip starts at (decision AA) |
| `/godot/cue/<id>/velocityFloor` — media | `d`, −40 (−120..0 dB) | rw | show | where a press of velocity one starts, and where a pressure of one takes the trim |
| `/godot/cue/<id>/pressure` — media | `T`, false | rw | show | sampler member: whether pressure on the pad rides the clip's trim while it is held, on velocity's scale |
| `/godot/cue/<id>/releaseFade` — media | `d`, 0.05 (0.. s) | rw | show | the fade to silence when a hold clip is released or a play-out clip is stopped by a second press; fifty milliseconds reads as a stop and does not click |
| `/godot/run/<id>/trim` | `d`, 0 (−120..12 dB) | rw | none | what a hand is adding to this run's level — the fader or pad on the strip it holds, and nothing else writes it. Starts at silence under a fader and at unity under a pad |
| `/godot/run/<id>/strip` | `s` | ro | none | the strip this run holds, when it is a sampler member on one: the strip's holder, read from the run's side |
| `/godot/run/<id>/held` | `T`, false | ro | none | whether a pad is down on this run, and so which press owns it (§16.5) |

**Four existing rows change.** `group/mode` gains `sampler` — *"sampler launches none of its
members: the hand does, from strips (PRD 3.27)"*. `slot/kind` gains `strip`, as the table says.
`/godot/slot/order` gains *", then the strips of every surface"*. And `run/pending` gains a word
that is not an identifier — *"…or the word voice, for a sampler member armed onto a strip while
every track is busy: it lands when one frees"* — for the reason §16.5 gives. **A strip's name is the
slot's**: a `Strip` carries the owners `slot` and `strip`, as a rack `Channel` carries `slot` and
`rackChannel`, so its name is `slot/name` and the `strip` owner must not declare one — the check M-B
added, that no element's owners declare a name twice, covers it.

**Published out of both halves of the tree, as a slot is** (§13.2). What the show decided, and what
its structure alone decides — a strip's `index`, `surface` and `endpoint`, a surface's `strips` —
comes from the document half, rebuilt when the show changes. What changes while nothing about the
show does comes from the runtime half: a strip's `holder`, `pending`, `target`, `word` and `cue`; a
run's `trim`, `strip` and `held`; a DCA's `trim`; and a surface's `connected`, `problem` and
`serial`, from a surface table handed to the tree as the MIDI ports are (`setSurfaces`, in
`setMidiPorts`'s shape; until PR 6.6 hands one over they read their defaults, and a virtual surface
reads connected regardless). **The trap in that split is silent, so it is written down**: a `none`
row on a document element is published by the document half WITH ITS DEFAULT unless that half skips
it and the runtime half emits it, and a row with no runtime branch at all publishes its default
without a word (`ParameterTree.cpp:1941`). A strip whose `word` read `free` for ever would pass
every test that only looked at a strip nobody had pressed.

### 16.3 The commands

Three creates, three run gestures, a rescan, and two new addresses for a verb that already exists.

| Command | Arguments | What it does, and what it records | What it refuses |
|---|---|---|---|
| `surface.create` | `<profile s> [name s] [id s]` | a `<Surface>` at the end of `<Surfaces>`, with the strips its profile implies: eight for `mcu` and `virtual`, sixteen for `d700` across its two ports and for `midiPads` (§16.2's note). Every identifier it draws — the surface's and each strip's — rides on the applied record in the order drawn, as `go` records the runs it makes, so a replay makes the same objects | a profile that is not one of the four, `bad-value`; under the edit lock, `locked` |
| `strip.create` | `<surface s> [id s]` | one more `<Strip>` at the end of a surface — of any profile, as PR 6.2 built it (§16.2's note) | `locked` |
| `dca.create` | `[name s] [id s]` | a `<Dca/>` at the end of `<Dcas>` | `locked` |
| `object.delete` | `<id>` | already generic; deleting a surface takes its strips with it | as today |
| `strip.press` | `<strip s> [velocity i 0..127]` | a hand on a sampler strip: launches the member on it, or applies its second-press rule if it is playing, and appends a step `p` to the list's history (§16.5) | a dca strip, `bad-value`. A free strip, a pending one, or one held from another origin: applied, and nothing happens |
| `strip.release` | `<strip s>` | the hand lifted: a hold clip fades to silence and stops, a play-out clip carries on | a dca strip, `bad-value`; with nothing held, applied and nothing |
| `run.arm` | `<run s>` | engine origin, submitted by the hook: the retry of a voice claim. The word `voice` leaves the run's `pending` and the member is armed again, now that a track is free | a run not waiting for a voice: applied, nothing |
| `surface.aim` | `<cue s>` | *(2026-09-25)* the media cue the rotaries edit on the EQ and Send pages, one for every surface; empty lets go. Held in the surface table and published at `/godot/surface/aim`, never stored, and no step of the show's history | a cue that does not exist, `unknown-id`; one that is not media, `bad-value` |
| `midi.rescan` | — | re-enumerates this machine's MIDI devices and rebinds the declared ports, on an explicit gesture and never on an idle tick, because enumerating blocks for milliseconds on Windows | — |
| `node.set` | `<address s> <value>` — two new live addresses | `/godot/run/<id>/trim` and `/godot/dca/<id>/trim`, answered in front of the document (§16.4); the applied record carries the value as an `f:` atom, and no undo transaction is opened | out of range, `bad-value`; not a number, `type-mismatch`. A write to a run that has finished is applied and ignored, as `run.kill` on one is |

**One reason is new**: `needs-strip`, in `needs-go`'s shape — a sampler member fired by name, by
`cue.fire`, a trigger or a `start` cue, while no armed group has put it on a strip (§16.5). A member
with a strip is pressed instead. And one old refusal gains a case: `standby.set` on a member of a
sampler group answers `not-a-stop`.

**The edit lock draws its line where decision W drew it.** The creates are document mutations and
refuse while the show is locked. A press, a release, `run.arm` and the trims are the show being
played rather than edited, and keep working — the trims because the door they go through sits in
front of the place the lock is asked (§16.4).

**The commands a surface sends that already exist** are the ones any client sends: `go`,
`run.stopAll`, `run.killAll`, `standby.previous` and `standby.next` from the transport buttons, and
`node.touch`, `node.release` and `node.releaseAll` from the faders (§16.6). A dca strip is never
pressed; on a Mackie surface its gate resets its DCA's trim to nought (plan decision 12), which is a
`node.set`.

### 16.4 One sum, one write verb

**A trim is an amount added to a level that leaves the level itself alone.** Pull a DCA down six
decibels and every cue marked with it plays six decibels quieter, while the levels written in the
show are exactly what they were. A group fade has worked this way since Phase 3 (decision O), and
§3.28 says a DCA *"adds terms to the same sum"*. So Phase 6 adds no arithmetic, only terms:

```
effective(run) = run.ownLevel + run.trim + Σ dcaChain (cueOf (run))
               + Σ over ancestor runs a: a.ownLevel + a.trim + Σ dcaChain (cueOf (a))
dcaChain(cue)  = trim (cue.dca) + trim (parent (cue.dca)) + …   (bounded by the DCA count)
```

`ownLevel` is what a fade writes, as today. `trim` on a run is what a hand writes, through the strip
the run holds. The DCA chain is the trim of the DCA a cue is marked with, plus its parent's, and so
on up; an ancestor group run brings all three of its own, which is how a group marked with a DCA
trims every member without anything flowing down to them.

**It is a sum, and that is the property that matters** — §3.28's words: *"Sums are
order-independent, which is the property that matters because cues arrive in whatever order the
operator pressed GO."* A DCA ridden before a cue fires and one ridden after arrive at the same
level. Adding decibels is multiplying gains, which is §3.6's *"nested trims compose
multiplicatively"* in the other unit. The chain is read into a map once per show revision —
`dcaChainOf`, rebuilt when `document.showRevision()` moves, as `applyRouting` is — so a tick pays
lookups and additions, walked no further than the number of DCAs so that a cycle past both refusals
could not hang one. The `approximatelyEqual` guard in `applyLevels` stays: a sum that did not change
sends nothing towards the audio thread.

**Both trims are `persist=none`, and the reason is §4.10**: where a fader sits at 04:12 is not a
decision about the show, it is what a hand is doing to it tonight. So a show opens with every DCA at
nought, and nothing a hand did yesterday is in the file.

**And a `persist=none` row cannot be written through the document at all — the finding that shaped
the door.** `Schema.cpp:360-363` files every `none` row among an element's derived attributes, and
`ShowDocument::setAttribute` refuses a derived attribute `read-only` before it asks anything else
(`ShowDocument.cpp:890-894`): *"A derived value is read-only by construction, whatever its row
says."* Its own comment a few lines further on anticipates the day a writable `none` row is resolved
there. Nor should the trims go through it if they could: a run is not a document object, and a trim
that reached `show.xml` would be a fader position saved as a decision. So the write is answered IN
FRONT of the document, by a dispatch the `node.set` handler consults first.

**The precedent went the other way.** `list/aim` is a runtime row with a command of its own —
`list.aim`, registered by the Runner — skipped by the document half of the tree
(`ParameterTree.cpp:987`) and emitted by the runtime half. Followed to the letter, the trims would
be written by `dca.trim` and `run.trim`, two commands, and the `node.set` handler would stay exactly
as it is.

**What decided it was who already speaks `node.set`** (plan decision 7). Three things do, and none
of them speaks a new command. The touch table is keyed by ADDRESS: `node.touch` and `node.release`
hold the address a fader writes, so a trim written by `dca.trim <id> <dB>` would be a write under
one name held by a touch under another, and the gate that stops the engine fighting a finger (§3.16)
would have nothing to gate. The page's generic inspector writes any writable node it is shown with
`node.set` and nothing else — §14.2's fifth rule, *a client assumes nothing about the parameter
table* — so a DCA's trim is ridable from the page the day its row exists, with no change to the
page. And every other client, a Max patch or `curl` or the desktop's own fader, gets it the same
way. About forty lines of dispatch buy all three.

**How the door works.** `registerDocumentCommands` gains a `LiveWrite` hook —
`std::function<std::optional<Outcome> (const std::string& address, const osc::Value&)>` — which the
`node.set` handler asks before `document.setAttribute`, for addresses under `/godot/run/` and
`/godot/dca/` only; for anything else it answers nothing and the document is asked, as today. Serve,
replay and the tests supply it. A run's trim is found in the run table, parsed against the
`run,trim` row — its type and its range, refused `type-mismatch` or `bad-value` exactly as the
document would refuse them — and written. A DCA's trim is checked against the declared DCAs and set
in a small table beside the runner (`cue/DcaTable.h`, std-only). A write to a run that has already
finished is applied and ignored, as `run.kill` on one is, because a surface a tick late should not
collect an `R` for every message it sent. `wfg tree` and `wfg validate` supply no hook: they have no
runs. The dispatch widens nothing else — `list/aim` written through `node.set` is still refused, and
a test says so.

**No undo transaction for a ride.** The before-apply hook that opens one transaction per applied
command (§14.9) skips `node.set` under `/godot/run/` and `/godot/dca/`, so `document/canUndo` stays
false after a hundred of them. §14.9 reserved a second undo domain for writes like these — *an
operator riding a level during a show must not be able to take back a cue rename by pressing Undo,
and must not have to* — and this phase does not build it either. A trim is nothing anybody decided,
so the document holds nothing for an undo to put back, and what an undo of a ride would put back is
a fader position: a motor moving because somebody pressed ctrl-Z. A ride is logged and replayed like
any `node.set`, which is the half of the promise that matters the morning after.

**And it keeps working under the edit lock**, which falls out of where the door sits rather than
from a rule of its own: the lock is asked inside `setAttribute`, of `persist=show` rows only, and
the trims never get there. That is decision W's line — show mode locks the editing and never the
mixing.

**A fade aimed at a DCA moves the DCA** (plan decision 16). A fade cue whose `dca` row names a DCA
moves `/godot/dca/<id>/trim` from wherever it is to the fade's destination; `target` is not read, a
DCA having no run to find, and `stopWhenDone` means nothing. A second fade on the same DCA takes
over from where the first had got to, as on a cue. It is a second row rather than a widened
`fade/target` because the `refers` machinery checks one kind per row; with both set, `dca` wins and
`wfg validate` says so. It is also how the devplan's *"a group DCA follows automation on motorised
faders"* is met in this phase: a fade cue moves the trim, the trim is a dca strip's `target`, and
the motor follows its target.

**Esc and double Esc leave DCA trims where they are** (plan decision 8). An abort ends runs; it does
not reset the desk, and after it the next GO sounds at the level the faders show. A run's own trim
ends with its run, and a DCA's resting state, §4.6's, is the nought every show opens at.

### 16.5 Strips — the fourth slot kind

**§13.2's fourth row, filled.** The slot table has carried a row for strips since Phase 4, with its
columns left for this phase:

| kind | pool declared by | typed by | released | a claim that finds none |
|---|---|---|---|---|
| `strip` | the layout: every `<Strip>` whose role is `sampler`, on every surface | role — a dca strip is pinned and never claimed | when the clip's run ends, however it ends | **waits** in the strip's pending queue, or **evicts**, when the arming group's takeover says so — and an eviction is a close |

**The claim is positional** (plan decision 3). The roster (`stripRoster`) is every sampler strip,
ordered by surface — `surface/order` — and then by index, rebuilt when the show's revision moves.
Member *i* of an armed group goes to strip *i* of that roster, across surfaces, in both takeover
modes. It is §3.27's *"in member order, left to right"* taken literally, and it has the property an
operator needs: the third member of a bank is the third fader, and nobody has to read a screen to
know it. A member beyond the last strip is not armed at all, and the group's row says *partially
armed*, §3.6's words for it. A member that pins its strip — *"the gunshot is always the rightmost
fader"* — is §3.27's *(proposed)* and not built (§16.10).

**Arming, in the handler and in the hook.** GO on a sampler group makes a group run and launches
nothing. The handler (`fireKind`'s group branch) sets the run playing, records its members, and —
when its takeover is `group` — marks every other live sampler group `closing`: a fact about the
model, written by a handler so that a replay writes it too. The members are the group's members in
order; no round is drawn, and `selection`, `play` and `loops` are ignored as a manual group ignores
them. Then, every tick, a sampler branch in the hook (`samplerTick`, ahead of the timeline branch in
`advanceGroups`) looks at each armed group, and every decision it takes leaves as a command:

1. **A closing group launches nothing new.** Every child armed and never launched is ended with
   `run.kill` — it never sounded, so ending it takes nothing from anybody — and a playing child
   plays on. When the last child has finished, the group's footer runs and it ends, exactly as a
   group ends today.
2. **Otherwise each member with no unfinished run gets one**: if its strip exists and the group has
   not lost it, `run.spawn` makes a run of the member there.
3. **A child waiting for a voice is retried** with `run.arm` the first tick a track is free.
4. **The fader edges**, below.
5. **Under strip takeover, a group that has lost every strip and has nothing unfinished completes.**

**A member re-arms after its run ends, any number of times.** The handler that ends a run releases
its slots (§13.2), so the tick after a member's clip ends, the branch finds that member with no
unfinished run and spawns it again on the same strip — until the group closes or loses the strip.
That is §3.27's *"any number of times"*, and it is also what makes every press a fresh run: nothing
of the last time the gunshot fired is left on the strip to confuse the next.

**The claim, in the handler** (`claimStripFor`, from `spawnChild` when the parent is a sampler
group). A free strip is taken. A busy one puts the claimant in the strip's pending queue, first come
first served as §13.2's queue always was, and under strip takeover adds the strip to the holder's
group's `lostStrips` (under group takeover that group is already closing). Then the member is armed
on a voice, and **this is where decision Z becomes code, in one branch.** Since Phase 3 `armMedia`
has failed a run at entry, `no-track`, when no track was free (`Runner.cpp:2443-2449`); for a run
marked as waiting for a voice (`waitsForVoice`) it now pushes the word `voice` onto the run's
`pending` and returns, and the hook's `run.arm` arms it again — idempotently — when a track frees.
Every other run fails at entry as before. A run pending a strip or a voice cannot launch, the rule
for any pending claim; a press on its strip is applied, does nothing, and the strip goes on saying
`pending`.

**Why `voice` is a word and not an identifier** (plan decision 4): `run/pending` lists slots by
identifier, and a track has none — it is not an object anybody declared (§13.2) — so the word stands
where an identifier would and says in words what is being waited for (§3.9e, §4.8). The retry is a
command because the hook decides and the handler applies: the hook sees the free track, and the
record lets a replay make the same arm without it.

**Where a fresh run's trim starts** (plan decision 5): at −120 dB under a fader and at 0 dB under a
pad. Under a fader, the fader is parked and lifting it is what starts the clip; under a pad there is
no fader to lift, and a press should sound. A pad pressed on a strip whose fader is parked — a V-Pot
press, the panel's pad — lifts the trim to nought, or to the velocity's level, so a press never
starts a clip nobody can hear. **And −120 at every new run is §3.9a's *"the start value is
reasserted at every handover"*:** the strip's `target` moves to the new run's trim, the panel and
the bridge fly the fader to the bottom there, and a fader left at −10 by a clip that ended on its
own never sits over a clip that has not started.

*Overruled by the author, 2026-09-23:* **a sample has an initial level, and its fader flies there.**
A new sampler-member row, `media/initialLevel` (dB, nought by default — a trim on the cue's own
level, so the default plays the clip as it was written), is where every fresh run's trim starts,
under a fader and under a pad alike. The fader waits there for the touch that starts the clip
(below). §3.9a's *"start value the fader flies to"* is therefore the member's, and *"reasserted at
every handover"* is still simply a fresh run. A press that is not the touch — a pad, an encoder, a
fired command — on a fader somebody has pulled to the bottom plays at the initial level, or at unity
if that is the bottom too.

**The two takeovers** (§3.27): what arming one bank does to a bank already on the strips, decided
once, on the group that arms — the scene change is where the designer is thinking about it. Both
follow §3.9e's second shared rule, **eviction is a close, not a kill**, which §13.15 left for this
phase with one instruction — *a close is exactly a release that waits* — and built as that:
`closing` and `lostStrips` on a group run are the whole of the state, and nothing ever stops a
sounding clip to make room. A playing clip always finishes before its strip switches, however it
finishes: its end, a release at the bottom, a stop cue, Esc.

- **`group`, the default: the whole group takes over.** Arming B closes every other sampler group. A
  closing group's idle members are ended at once, so their strips hand over at once; a member that
  is playing finishes, its strip says `closing`, and the strip's `pending` names B's member, which
  lands the tick the clip's run ends. When the last of the closing group's clips ends, its footer
  runs and it ends.
- **`strip`: only the strips B's members land on.** B takes strips one and two; A keeps three and
  four, spawns nothing more on the two it lost, and treats those two as a closing group treats all
  of its strips — an idle member there is ended, a playing one finishes and then hands over. Several
  sampler groups run at once, each owning what nobody has taken from it, and a group that has lost
  every strip has nothing left to offer and completes, as an emptied round completes a loop (§3.6).

**Refresh — and what it found on the way.** §3.27: *"A GO on a running sampler group re-issues the
claims of every member that has no strip, in the group's own takeover mode."* It has to land on a
check that is not there. Decision N says a second GO on a running group is ignored, and so does the
comment above the check in `armInternal` (`Runner.cpp:261`); but the check itself asks `liveRunOf`
only when the cue is media (`:276`), and the one guard a group has is for a PREPARED group, which is
adopted rather than started twice. So today a GO on a group that is already running starts it again
— a second copy of the scene on top of the first. The fix is pinned before anything is built on it:
a `GoTests` case that fires a live timeline group twice and expects one scene, then the check
extended to groups, then every replay fixture run again, because a fixture that happens to fire a
live group twice would replay differently, and that is worth knowing before the sampler exception is
laid on top. Then, for a sampler group, the live check is the refresh: the group's `lostStrips` is
cleared, its takeover runs again — under `group`, the others close — the GO returns the live run,
and the branch re-claims on the next tick. A refresh cannot make strips the layout does not have
(§3.9d's banking, unchanged); it only takes back what eviction took.

**Disarming, aborting, and the pointer.** A transport cue aimed at the group, or `run.stop` on it,
is the disarm (§3.27: no new cue kind): its children end, its footer runs, it ends, and its strips
come free. Esc and double Esc are what they are for every run (§4.4). `isManualGroup` stops
answering yes for a sampler group — which, being neither timeline nor automatic, it otherwise would
at the pointer's descent, `cue.fire`'s refusal and `fireStandby`. The standby never descends into
one: the cue list treats it as a row, GO on it moves the standby to the next sibling, and
`standby.set` on a member is refused `not-a-stop` — §3.27's flagged sentence, built as written, the
author's to overturn, and said so in the PRD where it is flagged. `ShowWalk.h`'s `timingInside` and
`groupLength` answer *not a chain* for a sampler group, so its length is unbounded, as a manual
group's is.

**Press and release.** `strip.press <strip> [velocity]` finds the run holding the strip; on a dca
strip it is refused, and with nothing on the strip it is applied and does nothing. Then, in order:

- **Held from somewhere else** — the run is held and the press comes from another origin: applied,
  and nothing happens. §3.27's second-surface rule, *the origin that started it owns it* —
  *(proposed)* there, and built here as the default.
- **Armed and not launched**: the trim is set — to `levelForByte (velocity, velocityFloor)` when the
  member's velocity mapping is on; otherwise to nought if the fader is parked, and left where the
  fader has it if it is not — and the launch is requested exactly as GO requests one on an armed
  cue. A `hold` member records who pressed: `held`, and the origin holding it.
- **Playing**: the member's `secondPress`. `restart` seeks it to the top, `noop` leaves it alone,
  `stop` is the release fade — §3.8's and §3.27's *(proposed)* third value, built as the default.
  That a seek to nought re-places a playing clip from the top without a click is what the
  implementation will confirm; if it does not, `restart` becomes a stop and a relaunch.

`strip.release <strip>` clears `held`. On a `hold` member it starts a fade to silence over
`releaseFade` that stops the clip when it gets there — the fade machinery a fade cue uses, started
with no fade cue behind it, which the implementation will confirm `beginFade` accepts — and on a
`playOut` member it does nothing. A `hold` clip cannot be pressed again by the hand holding it, and
while it is held a release from any other origin is a no-op too.

**One function for velocity and pressure.** `levelForByte (b, floor) = floor + (0 − floor) × (b − 1)
/ 126`: a byte of one is the floor, 127 is 0 dB, a straight line in dB between, and nought is the
floor as well. Pressure uses it while the pad is held and the member's `pressure` row is on, and the
bridge writes the result as `node.set` on the strip's `target` — so to the engine a pressed pad is a
fader somebody is riding (§16.6). A pressure of nought is IGNORED rather than mapped (plan decision
15): most pads fall to nought the instant the hit is over, and reading that as *pull the sound to
the floor* would make every hit a blip.

**Fader-start and fader-stop are engine rules, not a surface's.** The Runner's before-tick hook
decides them over two things every client already writes — the run's trim, and the touch table,
handed to the Runner with `setTouches` by serve and by nothing in replay — so the D700, the panel's
mouse, the page's slider and a script all get the same ones for nothing. Per sampler strip under a
fader it keeps whether the fader is parked, the last trim it saw, and whether anybody was holding
it:

- **parked** becomes true when the trim is at or below −118 dB and nobody holds the strip's `target`
  — *released at the bottom*;
- **start**: an armed member not yet launched, a parked fader, and a trim above −110 dB — the hook
  submits `strip.press` for the strip, engine origin, no velocity, and the fader stops being parked.
  A dip to the bottom while touched is therefore a ride, neither a release nor a press — §3.9a's
  *(proposed)* start edge, built as the default — and a play-out clip set to restart does not
  restart every time the operator dips;
- **stop**: a playing `hold` member, a trim at or below −118 dB, nobody holding it, and a fader that
  was held and has been let go, or has just come down from above — `strip.release`. §3.9a's
  fader-stop word for word: the bottom stops the cue *on release*, so riding through the bottom
  during a fade kills nothing.

The eight decibels between −118 and −110 are the hysteresis §3.9a asks for, *"or a parked fader
chatters"*. Both numbers live in one place (`faderEdge::parkedDb`, `faderEdge::startDb`), beside the
bridge's colour and motor constants (plan decision 14), so that what the bench and the room find
changes one line each; a debounce is a user preference and Phase 10's (§3.7). A replay runs no hooks
and needs none: it has the `strip.press` and `strip.release` records the hook submitted.

*Overruled by the author, 2026-09-23:* **a start is a touch.** *"The fader flies to this level,
waiting for the touch command to trigger playback."* The hook submits `strip.press`, engine origin,
when a new touch lands on the fader of an armed member not yet launched; the press keeps the trim
where the fader is, because the hand on the fader sets the level and the motor cannot move under
it. One touch starts one clip, and a touch on a clip already playing is a ride — so leaning on a
fader to bring a sound back up never restarts it. A move with no touch is a level set in advance and
starts nothing. **parked** and the −110 dB start threshold are gone, and with them `Run::ridden`,
which only the lift-start needed; **stop** is unchanged, §3.9a's fader-stop at the bottom on
release. `faderEdge::touchDwellTicks`, nought, is how long a touch must last before it counts: the
D700's faders report touches nobody meant — 58 of 81 in one capture landed within 150 ms of a
nearby button press (PRD §3.16) — so if reaching past a fader fires samples, the bench raises it, at
that much latency on every start. **A hand resting on a fader through a handover** holds the old
run's node and never the new one — the panel keeps its grab for the whole ride, and the bridge lets
go at the handover instead of moving its touch (§16.6) — so a bank changing, or a clip re-arming,
under a hand that did not move starts nothing.

*Added from the author's review, 2026-09-23:* **a sampler group is a window on the side of the
cues** — *"they can be triggered at any time by the user; the cue list goes on until we stop the
sampler group."* At the top of a list that was already so. Inside a sequence that plays itself it
was not: the sequence waited for each member to finish, and a bank only finishes when somebody stops
it. Now such a sequence arms the bank and moves on at once, and does not end — no footer, no next
round — while the bank it armed is live, so a scene's pads live as long as the scene and stopping
the scene takes them with it. A timeline or a manual group already lived that long.

**`cue.fire` and `trigger.fire` on a member** (plan decision 10). With a live run holding a strip, a
fire is a press with no velocity. Without one it is refused `needs-strip`: fired by name, a member
has nothing to run on until an armed group has put it on a strip. A fire has no release, so on a
`hold` member it plays the clip out — §3.27's *(proposed)* release-less trigger, built as the
default: *hold only means something to a trigger that can let go*. A `start` cue fires its target by
name, so it reaches the same rule.

**A press is a step in the list's history, and load-to-time skips it** (plan decision 9). The
handler appends `<tick>:<cue>:p` beside the `g`, `f` and `t` that a GO, a fire and a trigger leave
(§13.10); the velocity is not in it, because a step has no value field. Load-to-time's walk over the
history skips `p` steps, since where a hand was pressing is not a place the show can be put back to.
The live recorder keeps them and writes a press into a take as a `start` cue — which reaches a
member as a press — so a pad performance recorded once plays back, provided the group is armed when
the take plays. That is half of what the author asked of the live recorder on 2026-09-19; §16.10 has
the other half.

**What load-to-time does with a sampler group.** A landing after its row plans it as sounding — the
walk found no end to it — and seats the group run and its job like any other, and the branch re-arms
every member on the next tick; that a seated job reaches the branch is what the implementation will
confirm. Who pressed what is not reconstructed, and the solver says so with a confusion on the
group, `handLaunched` — *"nothing pressed"* — because a solver that says what it did not do is
better than one that guesses (§3.24). Strip ownership among several strip-exclusive groups depends
on the order they were armed in (§3.27), so whether seated groups re-claim in that order is part of
what the implementation confirms.

**`usage` on a strip answers empty.** §3.9c's edit-time analysis warns about two claims whose live
ranges overlap on one slot, and successive banks on the same strips are exactly that, on purpose: a
sampler claim declares its eviction policy and the overlap is the intended pattern (§3.27). So the
analysis has nothing to say about a strip, and says nothing.

**The words.** A strip is `free` with nothing on it; `dca`, or `unassigned` when a dca strip names
no DCA; `armed` when its member is ready and not launched; `pending` while a member waits for it, or
holds it and waits for a voice; `playing`; `held` while a pad is down on it; `stopping` during a
release fade; and `closing` when the group holding it has been taken over — whole, or on this strip
— and its clip is finishing.

### 16.6 The bridge

**Where it lives, and why there.** Beside the mount probe and the MIDI sender, in `engine/surface/`,
outside `Engine` and `Runner` — where §12.8 put the trigger matchers, for the same reason: the
engine reads no clock and owns no socket, and a surface is a socket with faders on it. Three parts,
each testable alone: a **codec** that turns bytes into typed events and back and knows nothing else;
a **profile table** saying what each kind of surface has, which button is a strip's gate and what
the transport buttons mean; and the **bridge**, which turns a surface's events into commands before
each tick and the tree into bytes after it.

**The codec** (`McuCodec`, PR 6.1) is pure — no JUCE, no state, one message in and one event out —
and its tests are byte vectors copied from `docs/D700_CONTROL_GUIDE.md` §3 and §4. Three of its
rules are the ones that bite. An encoder is **sign-magnitude**, bit six the sign, so 65 is −1 and
not −63; the guide ranks that as the most likely bug in any new integration. A pad controller's
messages are classified by the status byte, so a note-on of velocity nought stays one, as
`MidiInputs` already classifies it (§12.8's fourth point), and the bridge reads it as a release. And
colour is three note-ons on channels two, three and four at the element's own note, **blue last**
because the ring refreshes when blue arrives, refused for any note that is not one of the seventeen
RGB elements. The rest is the guide's tables: fourteen-bit faders, touch notes, a named button
table, both ring forms, the MCU scribble strip always padded to seven because its buffer is flat,
the D700's native rows with their row numbers one-based on the wire. The Mackie handshake is decoded
for the serial it carries and never answered (plan decision 6): nothing on the D700 needs the reply,
and the query alone yields the serial.

**The codec can emit six SysEx command bytes and no others** — `0x12 0x17 0x19 0x1A 0x00 0x02` — and
a test says so. A sweep of undocumented command bytes once put the D700's displays into a logo-only
state that took a full restart of the controller to clear
(`docs/godot-asparion-d700-protocol-0.1.md` §6), and the only way that comes back is a new byte in
an encoder written by somebody who did not know the history. `0x72` is on the protocol document's
safe list and not on the codec's: the colour it carries is MCU's eight, too coarse for timbre, and
§3.30 says a generic profile says so rather than approximating.

**The profile table** (`SurfaceProfile`, std-only, keyed by the profile word):

| profile | strips | each strip has | display | a strip's gate | endpoint |
|---|---|---|---|---|---|
| `virtual` | eight, more by `strip.create`; no port | what the panel draws: fader, pad, name, word, colour | the panel's own | the drawn pad | `absolute` |
| `mcu` | eight on one port; with an extender on a second port, sixteen, the other eight by `strip.create` | motor fader with touch, V-Pot and ring, buttons | two rows of seven | V-Pot press, `0x20 + n` | `absolute` |
| `d700` | sixteen, eight per port | the same, and an RGB surround on the encoder | 12 + 12 + 8, and a track number | V-Pot press | `absolute` |
| `midiPads` | sixteen, more by `strip.create`; a note each from `firstNote` | a pad with velocity and pressure | none | the note | `gate` |

**The gate is the V-Pot press, and SELECT is left alone** (plan decision 12). On the D700 an
element's identity is its button note — encoder three's V-Pot press, ring and colour all key off one
number — so the button that presses a strip is the one wearing its colour. On a dca strip the gate
resets the DCA's trim to nought.

**The transport.** PLAY is `go`. STOP is `run.stopAll` — Esc — and STOP again within 750 ms is
`run.killAll`, double Esc, so §4.4's first two levels are under the hand that is already on the
surface. ◀◀ and ▶▶ are `standby.previous` and `standby.next`. The bank and channel arrows do
nothing, because banking is §3.9d's decision to take with the hardware in hand, and REC does
nothing. The 750 ms is restated in the engine rather than shared: the client's is in
`model/Panic.h`, the engine links no client, and a comment on each points at the other. An encoder
moves its strip's `target` half a decibel a detent.

**Inbound, and why a tick's worth of fader is one write.** The MIDI callback thread does the least
it can: the bridge's consumer claims a surface's port — so a surface never fires triggers — and
pushes the port and the raw bytes into an inbox under a short mutex. The before-tick hook drains it,
decodes, maps port and bank to a strip, and submits in arrival order with the origin `surface:<id>`:
presses, releases and buttons as their commands; a touch as `node.touch` or `node.release` on the
strip's `target`; and a fader as `node.set` on it, COALESCED to one write per strip per tick — the
latest position wins — through a fader curve that restates `model/Fader.h`'s four points on the
engine side, a duplication noted in both. A moving fader sends a message every few milliseconds; the
tree shows one value a tick, so writing the rest would fill the log with positions nobody could ever
have seen. The inbox drains before `runner.beforeTick` in the same hook (plan decision 13), so a
tick's commands from the hand enter the queue ahead of the scheduler's. The origin, like `window`,
has neither `udp:` nor `ws:` in front of it, so the OSCQuery server's echo rule never mistakes a
surface for one of its own clients.

**Outbound, and why it costs nothing it does not send.** After the publish, the after-tick hook
hands the bridge the snapshot and the touch table. For each enabled, connected surface and each of
its strips, the bridge reads the strip's `target` and that node's value, its `word`, the cue's name,
short name and number, and the holder's timbre or the cue's colour; compares them with what it last
sent; and sends only the difference. Every send is `MidiSink::send`, which hands the bytes to
`MidiSender`'s worker thread and never does the I/O on the tick thread — a SysEx send busy-waits on
Windows, about 32 ms per hundred bytes (§12.11). The work is about sixteen strips times eight
lookups and compares a tick; the caches are allocated once, and nothing is allocated per tick beyond
the bytes actually sent. M29 measures it.

**Echo and touch, per strip and not per tick.** The WebSocket's echo rule is per tick: a push is
withheld from the origin that caused a change only when one origin caused the whole tick (§7a). A
surface cannot live with that — two faders moved by two people in one tick would each be told what
they had just done, and a motor would fight a finger — so the bridge suppresses per strip, which is
§7a's *"per-address attribution … belongs with Phase 6's real surfaces"* arriving for surfaces, and
only for them. No motor bytes go to a fader while the touch table has its `target` held by this
surface, and none when the value is the one that fader just sent. When this surface lets go, the
target's value is sent back once, so the fader ends up agreeing with the engine — the touch table's
own rule since PR 1.9. §3.16's *(proposed)* filter, a touch counting only once the fader has moved,
is not built (§16.10).

**A motor never crosses its whole travel in one message.** Driven end to end a fader hits its stop
at full speed, and the bottom of the range is where a parked channel lives
(`docs/D700_CONTROL_GUIDE.md` §4.1). So each tick it moves at most a twentieth of the travel towards
its target, 819 of 16 383, which is the guide's *"roughly 20 steps"* at the tick rate: a full-travel
flight takes twenty ticks, four tenths of a second. And it is never clamped short of the ends, in
the guide's own words: *"a fader that cannot reach −∞ misrepresents the desk, which is worse than
the wear."*

**Colour is quantised, rate-limited and re-asserted.** Eight levels per component, at most ten
writes a second per element — §3.30's *"no faster than about ten times a second"* — and at idle the
colour is written again every two seconds, because the firmware's idle animation takes the LEDs back
when nothing drives them (§3.16). While a clip sounds a strip shows the run's timbre; at idle, the
cue's authored colour — §3.30's *(proposed)* policy, built as the default, with no switch yet to
turn timbre off. The master dial keeps the idle colour, and a generic `mcu` surface drives no colour
at all. The rate and the interval are M27's and M28's to revise.

*Corrected at the author's direction, 2026-09-23:* **the timbre keeps its saturation** — *"it shows
how broad the spectrum is."* A timbre was read as HSL, and its lightness is its frequency axis, so a
bass bed lit a dark LED and a high effect a white one, washing the saturation out at both ends. It is
read now as the hue and the saturation at full brightness, silence dark — on the LEDs and on the
virtual panel's swatch alike. Brightness is left free for what the bench decides: modulating it
slightly with the amplitude or its variation, and a maximum-brightness switch for a booth in the
house that has to stay dark — both the author's *"we will see"*, neither built.

*And the touch across a handover, 2026-09-23:* a hand resting on a fader when its strip changes hands
lets go of the old node and holds nothing until it lands again, rather than moving its touch to the
new node — because a touch now starts a sampler clip (§16.5). The motor stays still under it all the
same, and what the hand moves goes to the new node, a ride with no touch.

**The displays.** An `mcu` strip shows the short name on its first row, and on its second the level
while a fader rides and the strip's word otherwise. A `d700` strip (PR 6.7) has three rows — the
short name in twelve; the level, as `-6.2 dB`, while a fader rides, else the word; the role or kind
in eight — and the cue number in the track-number field, sent on change; never `0x12` to a D700.
Every write is padded to its field. Which field goes on which row is *(proposed)* and the author's
to reassign, and so is one thing the rows imply: where nobody authored a short name the display cuts
the name to its width, the truncation §3.16 says a display should never have to rely on. A blank
cell or the cue number is the alternative, and either is a one-line change.

**Connecting, reconnecting, and M-B's debt.** `refreshSurfaces` runs in the after-tick where the
show's revision is checked, beside `refreshMountDeclarations` (§15.3), so a GO, which moves no
revision, pays nothing for it. It rebuilds the surface table from `<Surfaces>`, resolves each named
port's binding with its `rx` and `tx`, and sets `connected` and `problem`; a surface that becomes
connected is **re-asserted whole**, so one plugged in during a show is painted rather than left
blank until something changes, and an unbound port leaves it `connected = false` with a sentence,
sending and hearing nothing. **`rx` off gates what comes in and `tx` off silences what goes out**,
with the reason in `problem` (plan decision 18) — the symmetry with a network device (§15.4), and
the first code to read a MIDI port's `rx` and `tx` at all. And as far as surfaces need it, the rest
of M-B's debt: a port whose device rows change in the show is re-opened (`MidiInputs::openAs`; the
output unbound and bound again), enumerating on the show edit and never on an idle tick, and
`midi.rescan` is registered for the explicit gesture.

**The seam that lets a test play a surface.** `MidiInputs` gets one door, `route (port, bytes)`. It
asks the consumer first, under the mutex triggers already take, and if no surface claims the port it
converts the bytes with a JUCE-free `eventFromBytes` and matches triggers exactly as today. The JUCE
callback becomes one call to `route`, and `inject (port, bytes)` — public, and meant for tests — is
another. So bytes in, bridge, commands, runner and bytes out is a test with no hardware in the room,
which is how the standing test runs (§16.8).

**`wfg replay` installs no bridge and no consumer.** Everything a surface did is a record with its
origin on it, and a replay applies the records and needs nothing else.

### 16.7 The client

Two places, as decision AB drew them, both in the desktop client — and two edits to the page.

**`model/Surfaces` is what both read** — std-only, one pass over the snapshot gathered by
identifier, as `model/MidiPorts` reads ports — and a strip's row carries everything a column draws,
down to its colour: the holder's timbre while it sounds, the cue's colour otherwise. The gestures
are commands and nothing else — `createSurface`, `createStrip`, `createDca`, `pressStrip (strip,
velocity)`, `releaseStrip`, `touchNode`, `releaseNode` — each checked against the real registry in
`ClientTests`, the rule M2 set (§14.16).

**The Surfaces tab** is the MIDI tab's shape, three times over, and nothing on it is applied: every
cell is a `node.set` and the after-tick re-read makes it real — §15.7's rule for a tab with no
hardware to reopen. The **surfaces**: name; profile, as text once the surface exists; ports, one
chooser per bank from the ports the MIDI tab declares; preset; enabled; the state word; and a
delete. The **strips** of the surface picked above: index, a role menu, a DCA menu. The **DCAs**:
name, short name, a parent menu, a delete. The buttons are **ADD SURFACE**, **ADD STRIP** and **ADD
DCA** — three different words, because the tests find a button by its text and take the first. A
D700 row whose preset is empty says *"set the Configurator to Mackie"*.

**The virtual panel**, under Show → *Surfaces…*, is a window of its own and not a subject of the
foot (plan decision 11): the foot follows the pick and is about one cue, and the panel is the whole
desk and has to stay put while the operator picks cues. One column per strip across every surface,
in surface order and then by index: the number or short name, the state word, a colour cell, a fader
and a pad. **The fader is ridden the way a D700 fader is** — `node.touch` on the strip's `target`
when the mouse goes down, `node.set` as it drags, at most one per timer pass, and `node.release`
when it comes up, on `model/Fader.h`'s curve with the send mixer's drag — so the touch table gates
the panel exactly as it gates the hardware, and fader-start works from the mouse. **The pad is
pressed**: mouse down is `strip.press` with a velocity from one to 127, taken from where on the pad
the click landed, mouse up is `strip.release`, and the keys 1 to 8 are pads one to eight at velocity
100 while the window has focus. A dca strip's fader rides its DCA's trim. The origin is `window`,
and the panel reads only what `Client.cpp`'s timer hands it — the one call site that takes a
snapshot, which `check-client-boundary.py` counts.

**The inspector** gains the rows: on a media cue `dca`, `release`, `secondPress`, `velocity`,
`velocityFloor`, `pressure` and `releaseFade`; on a group `takeover` and `dca`; on a fade `dca`; and
`shortName` on every cue, right after `name`. A `dca` row is a menu of DCAs, as a port row is a menu
of ports. The sampler rows are greyed on a member whose group is not a sampler group — the shape
`stereoToMono` already has — and `secondPress` is greyed when `release` is `hold`, where it cannot
apply. Eight cues selected and one DCA chosen is eight `node.set`s, which is how §3.28's
multi-select assignment is met with nothing new.

**The cue list and the run pane say it in words.** A sampler group's row shows `pads` where a
timeline group shows its mark. In the run pane a sampler group's run reads, say, `armed 5/8 ·
pending 3`, counted from its children, and a member shows its strip's word and `on 3`.

**The page gets two list edits and nothing else**: the inspector's order of rows
(`views/inspector.js`) and Didi's flag for a sampler group (`views/didi.js`). Its generic inspector
already rides a DCA's trim through `node.set` (§16.4), which was the point of that choice.

The author judges it after PR 6.5, before anything is plugged in (decision AC).

### 16.8 The fixtures and drivers

**Two bundles.** `tests/fixtures/bundles/sampler/` is the show the phase is judged on: `tracks="2"`,
a virtual surface with four sampler strips and one dca strip, a DCA, a sampler group of four media
members with their `release`, `secondPress` and `velocity` mixed, a second sampler group of two
members with `takeover="strip"`, and a transport cue aimed at the first. Two tracks for four members
is deliberate: two members show `pending voice` the moment the group arms, so the voice wait is
exercised by the fixture itself rather than by a test that had to arrange it. Its `media/` is empty,
as every fixture's is, and the drivers generate their sounds in a temporary copy.
`tests/fixtures/bundles/surfaces/` is the standing test's rig: a virtual surface and an `mcu`
surface on one port, each with a dca strip on the same DCA.

**Two replay logs**, hand-written as `triggers.wfglog` was. `tests/fixtures/logs/dca-trim.wfglog`: a
`dca.create`, trim writes, a `go` on a marked cue, a fade aimed at the DCA, and the runs ending.
`tests/fixtures/logs/sampler.wfglog`: an arm, presses and releases, a takeover, a voice wait. Both
are replayed under both locales and must say *reproduced exactly*; the trims' `f:` values go through
the number formatter (§9's shortest round trip), which is what the second locale is there to catch.

**Two black-box drivers**, against the shipped binary. `tests/blackbox/phase6_sampler.py` runs
`--hosted --render` on a copy of the sampler bundle with generated sounds: it arms the group,
presses strips over OSC (`/godot/cmd/strip/press`), reads `run/state` and `slot/<id>/word`, finds
sound after a press and silence after a hold is released in the rendered file, and replays the
session's log. `tests/blackbox/phase6_surfaces.py` runs with no MIDI device at all: every hardware
surface must read `connected = false` with its sentence and the virtual one `true`, a press over OSC
must play, and the log must replay.

**The unit tests**, a file per part — `McuCodecTests`, `DcaTests`, `SamplerTests` (on the `GoTests`
rig with its fake player) and `SurfaceBridgeTests` — beside the document, slot, tree and client
tests the PRs extend.

**The standing test** is PRD §3.16's — *"two surfaces on different protocols bound to the same
node"* — which the devplan has carried since it was written. On `bundles/surfaces/`, the virtual
panel and an MCU surface share one DCA. A `node.set` from the window moves the MCU's motor; a fader
message from the MCU moves the DCA's trim and produces no motor bytes back while the fader is
touched, and exactly one resend when it is let go; and the touch table holds the address for
`surface:<id>` and not for `window`. The two protocols are the client's commands and Mackie's bytes,
and both end as `node.set` on one address, which is the whole of the claim.

### 16.9 What Phase 6 has to measure

Phase 5's numbering ended at M25.

| | what | what it decides |
|---|---|---|
| **M26** | a pad press to sound, in ticks and milliseconds: presses over OSC at known ticks, onsets found in the render | whether a pad feels like an instrument — and if it does not, the lever is the tick rate |
| **M27** | the colour write rate the D700 tolerates: a bench driver repainting all seventeen RGB elements at 10, 20 and 50 a second, the operator saying which rate first stutters | PRD §6.11's first colour question, and the ten-a-second limit |
| **M28** | how soon the D700's idle animation takes the LEDs back once the host stops painting — paint, stop, a stopwatch | §6.11's second, and the two-second re-assert |
| **M29** | the tick thread's cost of a full sixteen-strip refresh — M25's A/B, a `d700` surface unbound so nothing is sent, against the same surface bound to a loopback, reading `latenessMax` | whether the after-tick bridge fits beside the publish |

**M26 has an answer to expect before it is taken.** A press is applied in the drain of the tick it
arrives in and launched from the next hook, `1 + launchLatencyTicks` after it is applied (§13.1's
arithmetic), plus up to a tick in the inbox; a fader-start pays one tick more, because the start
edge is a decision the hook takes after the trim has been applied. A figure beyond that is a
finding. If the author finds a pad slow, the lever is the tick rate, parked at 50 Hz on 2026-09-18:
a faster tick shortens every term above.

**M27 and M28 need the unit on the bench and a person watching it**, which is why the devplan lists
them among the author's. Until they are taken the profile holds to ten colour writes a second and
re-asserts every two seconds, and both numbers live in one place (plan decision 14).

Each is an instrument that prints — `tests/blackbox/m26_pad_latency.py`, `m27_d700_colour_rate.py`,
`m28_d700_idle_resume.py` and `m29_surface_refresh_cost.py`, in `m25_window_cost.py`'s idiom — and
none is a ctest gate, for §14.14's reason: a wall clock on a shared CI runner is a flaky test that
teaches people to re-run the suite. The figures are recorded here and in PRD §6.11 when they are
taken, and the Mac mini's cross-checks are owed as they are for M22 to M25.

*Taken, and built, 2026-09-23.*

- **M26, a first reading on the wrong machine** — a Debug build, on a box shared with a compiler, at
  48 kHz with a 64-sample buffer, so `launchLatencyTicks` is 2. A press applied on tick T is placed
  by the hook of T + 1 and sounds 2.4 to 2.5 ticks after that: **3.4 to 3.5 ticks, 68 to 70 ms, from
  the press being applied to its sound, plus up to 20 ms queued for the tick boundary.** That is the
  answer expected above; the half tick past it is where a Debug tick thread runs inside its tick,
  behind the audio clock. The instrument counts apart the presses whose tick thread was far behind,
  which sound as late as it was, because those are the machine and not the path. It is slow for an
  instrument, and the levers are as written: the tick rate first; then placing a press's launch in
  the tick it is applied, which saves a whole tick but needs a press-shaped exception to *the hook
  decides*; the buffer moves `launchLatencyTicks` only above 320 samples. To be retaken on a Release
  build of a quiet machine.
- **M27 and M28** are scripts that talk to the D700 directly with `python-rtmidi` - colour notes on
  channels 2 to 4 and nothing else, no SysEx at all - so they need the library, the unit under the
  Mackie preset, and the Configurator closed. Not taken: the library is not on the build machine, and
  the unit is the author's.
- **M29 is not the script the table names.** The build machine has no loopback MIDI port, and the
  bridge paints only a connected surface, so an A/B through `serve` would have compared nothing with
  nothing. It is a skipped case in `SurfaceBridgeTests` - `wfg_tests --test-case="m29*" --no-skip` -
  that times the bridge's after-tick alone, for a sixteen-strip D700 with every strip filled, in the
  three shapes a tick comes in, after a warm-up it leaves out, and times the publish it sits beside
  the same way. Median per tick, Release: **0.10 ms** idle, **0.11 ms** with every fader riding,
  **0.10 ms** with every name changing - the cost is the dozen lookups each strip makes, not what
  changed - beside a stale publish of **1.5 ms**. So the bridge fits: a fifteenth of the publish,
  half a percent of the tick. **The finding is the publish, in Debug:** 4 to 4.5 ms of bridge beside
  **93 to 106 ms** of publish - a Debug build rebuilding this small show's tree takes five ticks to
  do it, which is why a Debug `serve` falls behind its audio clock (M26's late presses,
  `first_sound.py`'s 670 ms of `latenessMax`) and why every timing in this phase is to be retaken
  on a Release build. What it does not count is the sending, which is the MIDI sender's worker's
  and not the tick's.

### 16.10 The direction this phase does not build

**Banking** (§3.9d) — decided with the hardware in hand, as the PRD asks. The bank and channel
arrows do nothing, and a bank with more members than there are sampler strips arms what fits and
says *partially armed*. §3.9d's other rule holds by construction: a show written for a D700 that is
not in the room degrades rather than fails, because its strips still exist and the virtual panel
draws them all.

**A member pinning its strip** (§3.27 *(proposed)*). Derived from order is the half that exists
first.

**The touch-without-move filter** (§3.16 *(proposed)*). The resend on release is there; the filter —
a touch counting as adjusting only once the fader has moved past the hysteresis — is not, so a fader
brushed by an arm reaching for the master section is held for as long as it is touched. The author's
to decide with the D700 in hand.

**The dwell for faders without touch** (§3.9a *(proposed)*). Without it, anything that writes a trim
and never touches — a script, a slider on the page — is treated as a fader let go wherever it stops,
so bringing a hold clip's trim to the bottom stops it. §3.9a's fallback, *such a fader is play-out
only*, is the other reading, and the author's.

**A mapping per DCA assignment.** §3.28 has each assignment say what the DCA controls on that member
and with what mapping; this phase's mark is one identifier, and a DCA moves level, one to one in dB.
Composition by parameter type — multiplicative on an opacity, in metres on a position — arrives with
the parameters it composes, video's in Phase 8.

**§14.9's parameter undo domain** — still reserved and still not built (§16.4).

**Recording a ride into a take.** The author's words of 2026-09-19 expected it in this phase:
*"record all the cue starts (and controller level changes once this is implemented in the next
phase)"*. The presses are recorded (§16.5); the rides are not, because a take has nowhere to put
them — they want a lane recorded from the gesture (§3.10), which is its own piece of work. Every
ride is in the log meanwhile, so nothing played tonight is lost to a later recorder. Named here
because the author asked for it by phase.

**Fader-start outside a sampler group.** §3.7's fader-movement trigger on an ordinary cue, and
§3.27's show-long soundboard of fader triggers in a parallel list, are not built: a fader starts
what a sampler group has put under it, a soundboard is a sampler group armed once and never taken
over, and the devplan's *"a fader-start cue fires from the D700"* is met by a sampler member. Nor is
a start value other than silence (§3.9a): every handover flies to the bottom. *Overruled
2026-09-23: the start value is the member's `initialLevel`, and a touch is the start (§16.5).*

**Bindings in general** (§3.10) — automation modes, cue-scoped lifetimes, an explicit update-cue
capture. A strip's `target` is the only binding this phase has, derived rather than authored, and it
rides trims and nothing else.

**HUI, Stream Deck, meters, the rate endpoint class, and the Icon V1 and P1.** HUI is §6.10's and
not first; a Stream Deck is §3.16's triggering surface and a later profile; meters want a per-track
level readout the engine does not have; a SpaceMouse or a pedal has no strip to ride; and the Icon
units are the author's *"not in the scope here"* — new words in the profile table when they arrive.
**Timbre as a layout option** that can be switched off (§3.30 *(proposed)*) is not built either:
authored colour at idle and timbre while sounding is the default, with no switch.

### 16.11 Decisions to overrule early

Taken with the plan rather than by the author, each built on, and each cited above as *plan decision
N*:

1. **`Surfaces` and `Dcas` are two containers beside `Network`**, not a `Dca` under `Audio`; both
   cost every fixture a line.
2. **A strip's `index` is derived from its position and a surface's `strips` is a count**; strips
   are made by `surface.create` and `strip.create`, never by writing a number.
3. **The claim is positional**: member *i* to sampler strip *i* across surfaces, in both takeover
   modes. `group` closes the other groups whole and `strip` closes only the strips it takes,
   recorded as `lostStrips` on the group that lost them. A member beyond the last strip is unarmed
   and the row says *partially armed*.
4. **The voice wait is the word `voice` in `run/pending`**, the retry is `run.arm`, and `armMedia`
   does not fail `no-track` for a run marked as waiting for a voice.
5. **A fresh run's trim is −120 dB under a fader and 0 dB under a pad**; a pad pressed on a parked
   fader strip lifts the trim to nought, or to the velocity's level. *Overruled 2026-09-23: every
   fresh run starts at its member's `initialLevel`, and a touch is the start (§16.5).*
6. **No reply to the Mackie handshake**; the query is decoded for the serial only.
7. **Trims are written with `node.set`**, through a dispatch in front of the document, and not by
   `dca.trim` and `run.trim` commands — against the `list.aim` precedent, for the touch table and
   the generic inspector (§16.4).
8. **Esc and double Esc leave DCA trims where they are.**
9. **A press is a history step `p`, with no velocity**; load-to-time skips it and the live recorder
   keeps it.
10. **`cue.fire` and `trigger.fire` on a member with a strip are a press**; without one they are
    refused `needs-strip`.
11. **The virtual panel is a window of its own**, under Show → *Surfaces…*, and not a subject of the
    foot.
12. **The gate on `mcu` and `d700` is the V-Pot press**; SELECT is reserved.
13. **The bridge's inbox drains before `runner.beforeTick`**, in the same hook.
14. **The constants** (*the start threshold overruled 2026-09-23: a touch is the start*): parked
    at or below −118 dB and a start above −110 dB; colour at most ten
    writes a second and re-asserted every two seconds at idle; a motor at most a twentieth of its
    travel a tick. Each is named in one place, and revised by M27–M29 and the room.
15. **A pressure of nought is ignored**; one to 127 map on velocity's line (`levelForByte`).
16. **`fade/dca` is a second row**, rather than a widened `fade/target`.
17. **The D700's display fields**: name, level-or-word, role, with the cue number in the number
    field; meters not driven.
18. **`rx` off gates a surface's input and `tx` off silences its output**, with the sentence in
    `surface/problem` — the symmetry with network devices.

### 16.12 What Phase 6 built, against what §16 drew

*Written 2026-09-23, at the end of the night the phase was built in, on the local branch `phase6`
(not pushed: CI runs on `main`). Everything below was built and tested with no control surface in
the room; what only the D700 on the desk can settle is listed at the end, and it is the author's.*

**The draft held, and more closely than §14's did — because nothing in it was a layout.** Every PR
landed as drawn: the document objects (6.2, `f43d104`), the codec (6.1, `d2abe69`), DCAs and the
live door (6.3, `ace3040`), sampler groups (6.4, `6d62998`, heard in `e39571d`), the client (6.5,
`897621f`), the bridge (6.6, `9a345cf`), the instruments (6.7, `0b65760`). One step of decision AC's
order folded into another: the D700 layer arrived *with* the Mackie bridge rather than after it,
because a D700 is a row in the same profile table and the bridge's paint path asks the row what the
strip has — so the engine and the panel came first, as decided, and the two hardware steps came
together. §14.17's lesson — that a pull request whose subject is *how something
looks* stops being predicted by its number — has not been tested yet: the Surfaces tab and the
virtual panel are exactly that kind of work, and the author has not looked at them. Expect them to
change the way Phase 5's views did.

**Where the build disagreed with the drawing, the drawing lost, and each loss is small:**

1. **§16.2 — the objects.** A `midiPads` surface is made with sixteen strips, not eight: most pad
   controllers have sixteen. `strip.create` works on every profile, not only `virtual` and
   `midiPads`: a Mackie unit with its extender is one surface of sixteen, and refusing the second
   eight would make that surface impossible to declare. `surface/profile` is writable and changing it
   leaves the strips alone. The `refers` check now reads a space-separated value as several
   references, each checked — `surface/ports` needed it, and no identifier holds a space, so no
   single-valued row loses anything. `surface.create` checks every strip identifier it is handed
   *before* making anything, so a replay refused half-way cannot leave a surface short of strips.
2. **§16.4 — the sum and the door.** An out-of-range trim is refused `type-mismatch`, not
   `bad-value`, because that is what the document's own door answers for the same mistake on a
   stored row, and a client should not have to know which door a row goes through. A `DcaTable`
   sits beside the run table in all four verbs that build a Runner; only `serve` and `replay`
   install the live door. And a media run takes its own level only through the arm, so with no
   audio side a run's level stays nought — which the `dca-trim` fixture records honestly rather than
   hides.
3. **§16.5 — strips and sampler groups.** *Found by recording the fixture, not by any test written
   beforehand:* a pad press on a fader strip lifts the trim to unity with no start edge; let go before
   the launch was placed, the member stayed armed at unity, and the fader-edge rule then read a
   "parked" fader at unity and started the clip nobody pressed. A fader-start now needs a hand's
   *write* since the last tick (`Run::ridden`, set only by `node.set`), and any trim above the
   bottom is not parked. Also as built: a hold clip let go before its launch is placed has the launch
   withdrawn and stays armed; a member beyond the last sampler strip is left unarmed and the group
   says *partially armed*; a group with no strip available and nothing sounding completes; the
   horizon prepares a sampler group, GO adopts the prepared run, so the takeover is applied in
   `adoptPrepared` as well as `fireKind`; idle members of a closing group end with `run.kill`,
   sounding ones play out; voices go to waiting members before a finished member re-arms, never
   more asks a tick than free tracks. **Two things differ from what §16.5 promised, and both are the
   author's to rule on:** a stop cue aimed at a sampler group kills its sounding clips at once
   rather than fading each (the existing group stop kills children; Esc does the same), and GO on a
   running *non*-sampler group at the top of a list still starts a second scene — decision N says a
   live group is ignored, the comment beside `armInternal` says so too, and the code does not. Only
   the sampler refresh was added; the general fix waits for the author.
4. **§16.6 — the bridge.** Built as drawn, with the D700 layer in the same profile table rather than
   a second bridge. What the drawing did not say and the build had to decide: encoder detents and
   pad pressure are coalesced like faders, one `node.set` per strip per tick, or two detents in one
   tick read the same value and one is lost; a dca strip's gate reset is part of that same per-strip
   write, so a fader moved later in the tick wins; a motor's first move after connecting stops one
   step short of either end, since nobody knows where the fader is; the D700's middle row reads
   `-inf dB` at the bottom, as the client's `faderText` does; a surface not connected — including
   one whose port has tx off — is neither painted nor heard, but its port is still owned, so its
   traffic never reaches a trigger; a surface switched off in the show, or of a profile nobody knows,
   owns no port, so that port is an ordinary trigger source again (*a judgment call, flagged*); a
   port carries one surface, the first that names it, and the second says so in a sentence; a hand
   held through a handover lets go of the old node and holds the new one; the inbox drops MIDI clock
   and active sensing and stops at 8192 messages; bank 0's serial wins over bank 1's; the master
   dial's ring is not driven, since nothing in the show gives it a colour, so the firmware's
   animation keeps it. **The MIDI seam
   is new:** every arriving message goes through `MidiInputs::route`, offered to a consumer first —
   the bridge — and then to the triggers, and `inject` puts bytes on that road with no hardware.
   The bridge is owned by a shared pointer the input thread holds too, because that thread calls the
   consumer outside its lock and `midiIn` outlives everything declared after it. **Not built:**
   rebinding a port after start, and `midi.rescan` (M-B's debt): a surface's connected state follows
   the bindings made at start and the `rx`/`tx` rows, and re-patching a surface's cable needs a
   restart.
5. **§16.7 — the client.** A dca strip's pad on the virtual panel resets its DCA to unity rather than
   sending `strip.press`, which the engine refuses on a dca strip — §16.6's gate rule, applied to
   the mouse. A D700 with no preset written says *"set it to Mackie"*, shorter than the
   *"set the Configurator to Mackie"* drawn above, which the column cuts. The profile stays a
   chooser after creation (the row is writable), ADD STRIP works on every profile (as the engine
   does), strip rows have a ×, and a second port bank opens only once the first has a port, since
   a gap in the space-separated list would move the second port into the first bank. The inspector greys a row only when it is greyed for *every* picked cue (the
   first cue used to decide, for every greyed row, `stereoToMono` and the MIDI rows included); `dca`
   is never greyed, since any media cue can be trimmed; a header or footer cue is not a member even
   though the tree gives it the group as its parent; and a sampler group greys `advance`,
   `selection`, `play`, `loops` and `seed`, which it never reads. A sampler group with
   `advance=auto` is no longer offered for scrubbing. **Two old inspector bugs surfaced and were
   fixed:** the device and port menus were built and then hidden (the layout's menu test knew only
   `choice` and `busRef`), and a mixed multi-selection showed "(none)" — picking it to clear every
   picked cue sent nothing.
6. **§16.8 — fixtures and drivers.** `bundles/surfaces` gained a bus, because `serve --hosted`
   refuses a show with audio tracks and nowhere for them to go. `phase6_sampler.py` reads its render
   as arithmetic in `first_sound.py`'s way — a press at velocity 64 over a floor of −40 dB is
   *exactly* a tenth of the constant — and gives the fixture's copy a bus and routes, so the
   committed bundle stays the one `sampler.wfglog` was recorded from. The standing test lives in
   `SurfaceBridgeTests` as drawn; `phase6_surfaces.py` also carries the no-MIDI half of it.

**M26 was taken, once, on the wrong machine, and it already says something (§16.9).** On a Debug
build, on a box shared with a compiler, at 48 kHz and a 64-sample buffer: a press is *applied* on
tick T, its launch is *placed* by the next tick's hook, and it sounds `launchLatencyTicks` (two) and
about half a tick after that — **3.4 to 3.5 ticks, 68–70 ms, from the command to the sound, plus up
to 20 ms waiting for the tick boundary.** That is structural, not CPU: every term is a tick. It is
slow for an instrument. The levers, largest first: the tick rate (100 Hz halves every term, parked
on 2026-09-18 with exactly this in mind); placing a press's launch in the tick it is applied, which
saves a whole tick but needs a press-specific exception to *the hook decides*; the audio buffer moves
`launchLatencyTicks` only above 320 samples. A press whose tick thread was behind the audio clock
lands late by however far behind it was — the instrument counts those apart, and they are the
machine, not the path.

**M29 answered its question and raised a larger one.** The bridge's after-tick for a sixteen-strip
D700 with every strip filled costs 0.1 ms a tick in Release, beside a 1.5 ms publish: it fits, a
fifteenth of the publish. But the same instrument timed the publish in Debug at 93 to 106 ms — five
ticks for one publish of a sixteen-cue show — which is why a Debug `serve` runs behind its audio
clock at all, and why the late presses above exist. The author runs Debug builds; the D700 will be
plugged into one. That is not Phase 6's cost, but it is the first thing to look at if the surface
feels sluggish on the desk, and a Release build is the first thing to try.

**What only the author can do, with the D700 on the desk:** the four hardware clauses of the
done-when (a fader-start from the D700 with the audio armed, a group DCA following a fade on the
motor faders, provenance on the strip displays, a bank change finishing a playing clip before its
strip switches); M27 and M28 (`python-rtmidi`, the unit under Mackie, the Configurator closed);
the look of the Surfaces tab and the panel; the three rulings flagged above — the stop cue on a
sampler group, decision N for other groups, and whether a switched-off surface's port should fire
triggers; and whether the strip colour should follow the run pane's HSV reading of the timbre
rather than the HSL the row describes.

**The author's first look, the same day, before the D700 came out.** Four answers, built at once:
a sample has an initial level its fader flies to, and **a touch is the start** (§16.5) — which made
the bridge stop moving a resting hand's touch across a handover, or a clip ending under a hand
would have restarted itself; **the timbre keeps its saturation** on the strips (§16.6); and **a
sampler group is a window on the side of the cues**, which a sequence that plays itself had not
honoured (§16.5). The fixture `sampler.wfglog` was re-recorded under the touch-start, and the
black-box driver now touches a fader and watches the clip start. Still the author's: the stray
touches the D700 is known for, now that a touch fires a sample (`touchDwellTicks` is the lever);
brightness and its ceiling; the DCA faders' initial level — *"set at some point; we'll see what feels
most practical"*; and the rulings above.

**First contact with the unit, the same day, the author away and the D700 left connected.** The
bench show (`tests/blackbox/make_d700_bench.py`) served from a Release build with no audio device:
both ports bound at start under the names Windows gives them (`D 700`, `MIDIIN2 (D 700)`,
`MIDIOUT2 (D 700)`), the surface connected at once, and **the D700 answered the Mackie handshake**
— the serial it reports is `D700 MA` — so the road runs both ways. Bank A armed, its faders flew to
their four initial levels, fader 3 was ridden from the network and the Band DCA faded down and back
up, and in all of it **the unit sent nothing of its own accord**: no position echoed from a moving
motor, so nothing the bridge could mistake for a hand. `latenessMax` over the session was 320
samples (6.7 ms), which is M29's bound half, taken in serve rather than in-process. What only
somebody looking at the desk can say — the displays, the rings, the colours, the motors' feel, a
touch — is still the afternoon's.

## 17. Phase 9a — EQ on media cues, the plugin sandbox, and VST inserts: what the tree, the commands and the log gain

Written on 2026-09-23, before the code, as §11 to §16 were: the approved Phase 9a plan drawn as a
text the pull requests 9a.1–9a.11 can be reviewed against rather than against memory. It is drawn
against `main` at `c3d75fe`, where Phase 6 landed whole and the surface-pages draft with it. Rows
reach `docs/parameters/godot-parameters.csv` with the PR that implements each of them, never
before. Where this section and the code come to disagree, §17.12 at close-out says which won.

The request was the author's, on 2026-09-23: *"I would like to look into EQ on media files and
VST inserts too. The rotaries of the hardware controller will be used at times to control the EQ
and VST parameters. There are toggles to switch to 'EQ' and 'FX' modes on the D700 for
instance."* The rotaries and the toggles are the surface pages of
`docs/godot-surface-pages-draft-0.1.md`, designed the same day and built afterwards by another
session on top of this phase; what that session needs from this one is §8 of that draft, and it is
the contract every subsection below serves: **every EQ and insert parameter is a node under its
cue**, carrying its value and published beside its name, short name, default, range or steps,
bipolar flag and the plugin's own value text; **written with `node.set`** from any origin, the
touch table gating it, undo coalescing a turn into one step, any thread hop the engine's and never
a client's; **the insert order readable**, each with its plugin's name; and **names and ranges
readable without a playing instance**.

**What the phase is, before any of its names.** Until now a media cue's sound was its file, a
level, and where it went. Phase 9a gives the cue a sound of its own: an **EQ** every media cue has
from birth, four bands and two filters, flat until somebody shapes it; a **plugin set** the show
declares once, as it declares its tracks — the third-party processors this show uses, loaded
when the show opens and carried, switched off, on every voice; and on each cue the plugins of
that set it **switches in** and the **values** it sets on them. A hand turning a rotary on any of
it writes the cue — a decision, saved, undone as one step — and a clip already sounding follows
within a tick. And because a third-party plugin is the one thing in the process that Go.dot did
not write, it runs **outside the process**, in a child that can die without taking the show down.

Four decisions the author took with the plan shape it — **AD**, **AE**, **AF** and **AG** in §9,
after AC — and §17.1 says what each means for a show:

| | decision | what it shapes |
|---|---|---|
| **AD** | the EQ is Go.dot's own — a fixed stage on every voice, written from the tick thread like the level | §17.5, nineteen `media` rows, and no message-thread hop anywhere in the EQ |
| **AE** | inserts are a chain on every voice track — the show's plugin set on every voice, a cue enabling and setting | §17.2's `Plugins` and `Fx` objects, and the N × P instances §17.9 measures |
| **AF** | the out-of-process proxy is built first — Phase 9's sandbox, pulled forward | §17.6, and the order of every pull request after the EQ |
| **AG** | the scope: EQ, inserts, the §8 contract, the inspectors, tests and measurements — not the pages, not the panel | §17.8 stops at the inspectors; §17.10 names the pages as the next session's |

Twenty-one further decisions were taken with the plan rather than by the author. They are numbered
in §17.11 and cited in place as *plan decision N*, each an implementer's call written down so that
it can be overruled early rather than late — §14's convention, kept. Where this phase builds
something the PRD still marks *(proposed)*, it says so where it builds it, and PRD §6.9 says which.

**Where it starts, in the code rather than in the plan.** One custom plugin sits at the end of
every voice track, `CueOutputPlugin` (`src/wfg/engine/audio/AudioHost.cpp:371-385`), registered
with `createBuiltInType` at `:203`; its level and routing matrix are atomics on `CueMatrix`,
written from the tick thread by `HostPlayer::setLevelDb`
(`src/wfg/engine/audio/HostPlayer.cpp:151-157`) — one relaxed store, and the audio thread
interpolates. `cue::Player` (`src/wfg/engine/cue/Runner.h:210-309`) names no Tracktion type;
`ArmRequest` (`:164-201`) is a plain value that crosses to the message thread, where
`HostPlayer::serviceArms` runs on a 10 ms timer (`HostPlayer.cpp:38, 68-126`) and every arm
already rebuilds the graph, `source` being on Tracktion's restart list. `Runner::applyRouting`
(`src/wfg/engine/cue/Runner.cpp:6587-6640`) pushes a document edit to every sounding run under a
`showRevision` gate, which is the precedent for pushing an EQ or an insert edit. A door in front
of the document answers `node.set` for the live rows (`src/wfg/engine/cue/LiveRows.{h,cpp}`,
composed at `DocumentCommands.cpp:631-663`), and `ShowDocument::resolve` accepts exactly four-part
addresses (`ShowDocument.cpp:656-731`) — which is why every address below is flat and the nested
shape the pages draft sketched in §8 is not used. No `JUCE_PLUGINHOST_*` is compiled;
`cmake/WfgOptions.cmake:22-24` reserved the one place for it. Delay compensation is off
(`AudioHost.cpp:302`). Tracktion's own equaliser takes a lock on the audio thread
(`tracktion_Equaliser.cpp:271`) and its `AutomatableParameter::setParameter` asserts the message
thread (`tracktion_AutomatableParameter.cpp:1412, 1466`); neither is used by anything below. The
sandbox was proved in spike 07 (`spikes/spike07_proxy_plugin/main.cpp`,
`docs/spikes/spike07-proxy-plugin.md`): a custom plugin type, a shared-memory round trip at 0.9 µs,
a 250 µs deadline held, a child killed mid-playback and the playback surviving. Spikes never
migrate into `src/`; the design does.

Three rules hold over all of it. **GO never blocks** — no plugin is ever instantiated after the
show opens; the children come up beside the transport, which is already playing, and a cue that
lands before they are ready plays dry and says so. **The audio thread is a lipogram** — the EQ and
the proxy allocate nothing, lock nothing and enter the kernel nowhere; the one concession, a clock
read every sixty-four turns of a bounded spin, is the one PRD §3.18 already recorded. And **a turn
writes the cue** (PRD §4.10): what a rotary does to an EQ band or a plugin parameter is a decision
about the show, saved, undoable and coalesced, and never a live override of the kind a fader's
trim is.

### 17.1 The four decisions the author took (2026-09-23)

Asked directly, one question each with a recommendation beside it. Two recommendations were taken
and two were declined; where a recommendation was declined, the reason the author's answer is the
better product is given with it, and the cost is stated rather than hidden.

**AD — the EQ is Go.dot's own.** A fixed stage on every voice track, in `CueOutputPlugin`'s shape:
a custom Tracktion plugin type registered once, no automatable parameters, its settings atomics
that the tick thread writes exactly as it writes the level, so a rotary's turn reaches the sound
without crossing to the message thread and without a lock. High-pass, low-pass, four parametric
bands each with a frequency, a gain and a width; the settings live on the cue, and every gain
rests at 0 dB, which is §4.6's resting state by construction. The alternatives were Tracktion's
own four-band equaliser — a lock on the audio thread, parameters writable only from the message
thread, and no high-pass or low-pass — and an EQ plugin chosen per cue like any insert, whose
parameters would be whatever that plugin exposes, so that an EQ page could not be laid out for
the hands. Known parameters are what the pages draft's §7.1 was drawn for: three encoders a band.

**AE — inserts are a chain on every voice track.** The recommendation was PRD §3.18's rack
channels as written — a channel being a track with a plugin chain loaded at open, a cue claiming
one and playing on it, cues sharing instances and carrying their own settings. The author chose
the other shape: **the show declares a plugin set**, as `Show/Audio/@tracks` declares polyphony;
**every voice carries the whole set**, each plugin instantiated when the show opens and switched
off; **a media cue says which of the set it switches in and carries its own values**; at arm the
voice gets them, and while the cue sounds a `node.set` reaches the voice live. This is the stack
PRD §3.18 proposed on 2026-09-07 — *"a channel may hold a stack of candidate plugins, all
bypassed, and a cue enables the ones it wants"* — answered yes, and placed on the voices rather
than in a rack. What it buys: no allocation to fail. Every voice has every plugin, so a cue never
plays dry because a channel was busy, and a designer never counts channels; a cue's inserts are a
property of the cue and nothing else, which is §4.12's *content describes output* read
literally. What it costs: N × P instances, one of every plugin on every voice, their memory and
their time at load (§17.9's M34 puts numbers on it), and a plugin set that is fixed once the show
opens (§3.25's rule about the graph), so adding a plugin to the set mid-session takes effect at
the next open. Phase 4's `Media/Insert` and `Rack/Channel` — a cue claiming a rack channel — stay
exactly as they are, bookkeeping for the live-input rack that is still Phase 9b's.

**AF — the out-of-process proxy is built first.** The recommendation was to host VST3 and AU in
process now, behind the one build switch, and to build §3.18's sandbox with Phase 9. The author
chose the sandbox first. The reason is §3.18's own: *"when a plugin dies, the show survives"*, and
a plugin that is not Go.dot's is the one thing in the process whose failure nobody here can
prevent. So the shape spike 07 proved is built: a **proxy** — a custom Tracktion plugin type on
every voice, one per plugin of the set — that hands each block to a **child process** through
shared memory and waits for it with a hard deadline, on a bounded spin; a miss passes the dry
block through; a strip that keeps missing is **marked failed and stops being called**, which is
the requirement spike 07 found; and the child hosts the real plugin, one instance per voice. The
cost is order and time: every VST insert waits on the proxy, the proxy is most of what devplan
Phase 9 drew, and it is weeks. The EQ, being Go.dot's own, waits on none of it. One consequence
is a gain: a plugin behind the proxy has no Tracktion `AutomatableParameter` at all, so its
parameters travel tick thread → shared memory → child, and §3.4's message-thread handover — the
one spike 05 measured — applies only to a plugin hosted inline, which this phase does not build.

**AG — the scope.** The EQ, the inserts, the four items of the pages draft's §8, the desktop and
page inspectors that edit them, the tests and the measurements. Not the surface pages — the focus
row, the page model, the EQ and FX pages on the D700 — and not the virtual panel's rotaries: those
are the pages session's, built on this, in the order the author set in the draft's §11.

| decision | the PRD sentence it answers or amends | written where, 2026-09-23 |
|---|---|---|
| **AD** | §3.18's *"a built-in plugin set covers basic needs and runs in-process"* — the first of that set | §3.18 in place, and §6.9 |
| **AE** | §3.18's *(proposed)* bypassed stack — yes, on the voices | §3.18 in place, and §6.9 |
| **AF** | §3.18's *"third-party plugins are hosted out-of-process by default"* — the order, and the parameter path | §3.18 in place, §3.25's *"nothing before Phase 9"*, and §6.11 |
| **AG** | the pages draft's §8 and §11 | §7.3 of that draft, and its §8 at close-out |

### 17.2 The objects and their rows

**A media cue's EQ is nineteen rows on the cue, not an object of its own** (plan decision 1).
Every voice carries the EQ from load, so every media cue has one whether or not anybody has
touched it, and an object that had to be created before its first write would say otherwise. Flat
rows on `media` give a page a direct address from the focus cue's identifier — `/godot/cue/<id>/
eqB2Gain` — with no scan of a second owner and no `eq/cue` indirection; `node.set` and undo's
coalescing work through the document's own door on the first day; and the canonical writer omits
an attribute at its default, so a flat EQ writes nothing into `show.xml`. The price is nineteen
nodes on every media cue in the tree, and M30 records what they cost a publish.

**The show's plugin set is a container under `<Audio>`**, `<Plugins>` holding one `<Plugin>` per
processor the show uses, published at `/godot/plugin/<id>` with `/godot/plugin/order` beside it —
the `Dcas` shape. It sits under `Audio` beside `Bus` and `Rack` because it is an audio fact of the
rig, and it is made on demand by the first `plugin.create`, as `createRackChannel` makes `<Rack/>`,
so no fixture gains a line. An entry names the plugin the way the machine's scan names it, keeps
the name and the file path for the person who has to find it on another machine, and names a
**preset** — a file under the bundle's `plugins/` folder, never the bytes in a row (plan decision
17): a `.vstpreset` is tens of kilobytes, the tree publishes every `show` row, and a page polls the
tree ten times a second; §14.11's rule that a client never hands an unbounded engine string to a
fixed-size control is a rule about what the engine publishes too. The preset arrives the way media
does (decision Y): a native dialog, a copy into `plugins/`, one `node.set` naming the file.

**A cue's insert is an `<Fx>` child of `<Media>`** (plan decision 2), owner `fx`, published at
`/godot/fx/<id>` with a derived `cue`, the `Send` shape exactly (§13.3). `Insert` was not
available: it is Phase 4's rack-channel claim and stays so. `Plugin` under `Media` would have
collided with `Audio/Plugins/Plugin`, since `ShowDocument::ownerForElement` is keyed on the element's
name alone. `Fx` is the author's own word for it — the D700's button, the pages draft's §7.2. An
`Fx` names one plugin of the set, says whether it is switched in, and holds the values this cue
sets — **only the ones somebody changed**, as a sparse list; a parameter the cue does not mention
rests at the set entry's preset, which is what the plugin was put into at load.

**The nodes a plugin's parameters become are generated, not rows.** A plugin's parameter list is
known only from an instance, and its length differs from one plugin to the next, so the CSV cannot
name them. Two kinds of node are built by the tree from the machine's **catalogue** (§17.7): under
every `Fx`, one `p<n>` per parameter — writable, a normalised number in 0..1, described by the
parameter's name, carrying its unit and, for a stepped parameter, its step texts as `enumValues` —
and beside each a read-only `t<n>`, the plugin's own text for the value the cue holds. Under every
`Plugin` entry, `param/<n>/{name, shortName, default, min, max, steps, unit, bipolar}`, read-only,
the catalogue itself; and under `/godot/plugin/known/<n>/{name, identifier, format, manufacturer}`
what this machine's scan found, so a client can offer them. The `p<n>` nodes are what §8's first
two items ask for; `param/<n>` is what its fourth asks for.

**The containment**, in `Schema.cpp`'s table:

```
{ "Audio",   false, { "Bus", "Rack", "Plugins" },                         { "audio" } },
{ "Plugins", false, { "Plugin" },                                         { "plugins" } },
{ "Plugin",  true,  {},                                                   { "plugin" } },
{ "Media",   true,  { "Route", "Send", "Feed", "Insert", "Range", "Trigger", "Fx" }, … },
{ "Fx",      true,  {},                                                   { "fx" } },
```

`<Plugins>` is written at a fixed place under `<Audio>` — after the last `<Bus>`, before `<Rack>` —
whichever was created first, so that the canonical bytes of a show do not depend on the order two
containers happened to be asked for.

The rows, grouped by where they are published. Every one carries the panic policy `park`, and its
resting value is its default.

| Node | Type, default | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/cue/<id>/eqOn` | `T`, true | rw | show | whether the cue's EQ is in the signal at all; off, every band is skipped and the audio passes untouched |
| `…/eqHpf`, `…/eqLpf` | `T`, false | rw | show | whether the high-pass or the low-pass is in — a flag, not a frequency parked at the edge, because a rotary's click switches a filter in and out |
| `…/eqHpfFreq` | `d`, 80 Hz (20..2000) | rw | show | where the high-pass turns over, second-order Butterworth |
| `…/eqLpfFreq` | `d`, 12000 Hz (1000..20000) | rw | show | where the low-pass turns over, second-order |
| `…/eqB1Shape` | `s`, `peak` (`peak\|lowShelf`) | rw | show | what band one is; the low band is the one that wants to be a shelf |
| `…/eqB4Shape` | `s`, `peak` (`peak\|highShelf`) | rw | show | what band four is |
| `…/eqB{1..4}On` | `T`, true | rw | show | whether the band is in the signal; off keeps its shape, frequency, gain and width, so on brings back the same band — the press of a gain rotary on a surface's EQ page (author, 2026-09-25) |
| `…/eqB{1..4}Freq` | `d`, 100 / 500 / 2000 / 8000 Hz (20..20000) | rw | show | the band's centre, or the shelf's corner |
| `…/eqB{1..4}Gain` | `d`, 0 dB (−24..24) | rw | show | the band's gain; nought is the band out of the signal, what every band starts and rests at |
| `…/eqB{1..4}Q` | `d`, 0.7 (0.1..10) | rw | show | the band's width, higher narrower; on a shelf, how steep the corner is |
| `/godot/cue/<id>/fx` | `s` | r | none | the identifiers of this cue's switched-in `Fx` children in chain order — §8's third item |
| `/godot/plugin/order` | `s` | r | none | the set's identifiers in document order, which is the order of the chain on every voice |
| `/godot/plugin/<id>/name` | `s` | rw | show | what it is called on screen and on a scribble strip; the scan's name when added, editable |
| `…/identifier` | `s` | r | show | the plugin as the machine knows it, JUCE's identifier string; written only by `plugin.create` |
| `…/format` | `s` (`VST3\|AU\|LV2`) | r | show | which kind; LV2 listed and not built |
| `…/path` | `s` | r | show | where the file was on the machine it was added from |
| `…/preset` | `s` | rw | show | the `.vstpreset` under the bundle's `plugins/` the plugin is put into at open, or empty for its defaults |
| `…/state` | `s`, `unloaded` (`unloaded\|loading\|loaded\|missing\|failed`) | r | none | what became of it tonight, in a word — never colour alone |
| `…/problem` | `s` | r | none | why it is not loaded, in one sentence |
| `…/latencySamples` | `i`, 0 | r | none | the delay the plugin itself declares, uncompensated: a cue through it is late by this much while it is in |
| `…/paramCount` | `i`, 0 | r | none | how many parameters the catalogue knows; the `param/<n>` nodes count to it |
| `…/param/<n>/…` | generated | r | — | name, short name, default, min, max, steps, unit, bipolar — §8's fourth item |
| `/godot/plugin/known/<n>/…` | generated | r | — | this machine's scan: name, identifier, format, manufacturer |
| `/godot/fx/<id>/plugin` | `s`, `refers=plugin` | rw | show | which plugin of the set this cue switches in; one `Fx` per plugin per cue |
| `…/enabled` | `T`, true | rw | show | whether it is in this cue's signal; off keeps the values and costs the voice nothing |
| `…/values` | `s` | rw | show | the values this cue sets, `index:value` pairs, normalised, only the ones somebody changed |
| `…/p<n>` | generated `d`, 0..1 | rw | — | one parameter: what a hand writes, one address a turn; the door of §17.3 puts it into `values` |
| `…/t<n>` | generated `s` | r | — | the plugin's own text for that value, from the catalogue's table |
| `…/cue`, `…/name`, `…/index` | `s`, `s`, `i` | r | none | the cue it belongs to; the plugin's name; where it sits in the chain |

The CSV rows land with the PRs that implement them — the nineteen `media` rows with 9a.2, the
`plugins`/`plugin` rows with 9a.4, the `fx` rows and `media/fx` with 9a.8 — and are quoted in full
in the plan; this table is what they mean.

### 17.3 The commands

Three creates, one reset, one restart, one engine record, one new door for a verb that already
exists, and three verbs of the binary.

| Command | Arguments | What it does, and what it records | What it refuses |
|---|---|---|---|
| `plugin.create` | `<name s> <identifier s> <format s> <path s> [id s]` | a `<Plugin>` at the end of `<Plugins>`, made under `<Audio>` if the show has none. All four words explicit, so a replay on a machine that has never scanned needs no known list | `locked` |
| `fx.create` | `<cue s> <plugin s> [id s]` | an `<Fx>` under a media cue naming one plugin of the set, switched in, with no values | a cue that is not media; a second `Fx` naming the same plugin — `bad-value`, `createSend`'s word for a second send into one bus; `locked` |
| `eq.reset` | `<cue s>` | the nineteen EQ rows back to their defaults in one transaction, so Undo takes the whole reset back as one step — the pages draft's double-click | a cue that is not media; `locked` |
| `plugin.restart` | `<plugin s>` | brings a failed entry's child back: relaunched, preset re-applied, every sounding run's values re-armed | an entry that is not failed: applied, nothing |
| `plugin.failed` | `<plugin s> <problem s>` | engine origin, submitted by the proxy host once per failure: the record that a child died or stopped answering, so a replay knows the cue went dry there. A no-op on replay, as `run.failed` is | — |
| `object.delete` | `<id>` | already generic: a `Plugin` entry or an `Fx` | as today |
| `node.set` | `<address s> <value>` — one new door | `/godot/fx/<id>/p<n>`, answered in front of the document (§17.4): the value is put into that `Fx`'s `values` inside the ordinary transaction, so it is undone like any edit and coalesced on the `p<n>` address | not a number or outside 0..1, `type-mismatch`; no such `Fx`, `unknown-id`; `n` beyond what the catalogue knows, `bad-address` — and when the catalogue does not know the plugin, any `n` is accepted (plan decision 8) |

The three verbs of the binary: `wfg plugins --scan[=vst3|au] [--path=<dir>]` scans this machine
out of process and keeps what it found where Tracktion keeps it; `wfg plugins --list` prints it;
`wfg plugins --catalogue=<identifier>` loads one plugin in a child and writes its catalogue. And
one the operator never types: `wfg plugin-host --region=<path> --plugin=<identifier>
--instances=N --channels=C --parent-pid=P [--preset=<file>]`, the child, the same binary
re-invoked (§17.6).

**The edit lock draws its line where decision W drew it.** The creates, the reset and every
`node.set` on an EQ row or a `p<n>` are document mutations and refuse while the show is locked —
which is the pages draft's own rule, §5.5: the editing pages go dark under the lock. `plugin.
restart` and the engine's `plugin.failed` are the show being run, and keep working.

### 17.4 The parameter path, end to end

1. **A hand** — the desktop's EQ or FX panel, the page's generic inspector, later a rotary —
   sends `node.set <address> <value>`. The touch table gates it per address (§16.4); undo joins
   consecutive writes to one address from one origin inside twenty-five ticks into one transaction
   (`ShowDocument.h:505-537`), so a turn is one step and two rotaries are two.
2. **The document.** An EQ row is an ordinary `persist=show` row: `node.set` reaches
   `ShowDocument::setAttribute` and nothing new is in its way. A `p<n>` is answered by the **FxWrite
   door** (`cue/FxRows.{h,cpp}`, `liveWriteFor`'s shape with the opposite intent — it writes the
   document): it parses the number with the row's own parser, reads the `Fx`'s `values`, rewrites
   entry `n` with the canonical number formatter, and writes the row through the ordinary door,
   inside the transaction the hook opened. `isLiveWrite` stays false for it; unlike a trim, this IS
   a decision.
3. **The tick thread.** `Runner::beforeTick` gains `applyEq` and `applyFx` beside `applyRouting`,
   under the same `showRevision` gate: for every sounding media run, read the cue again through the
   schema `Reader` (`ShowWalk.h:98-165` — never a raw `ValueTree` read, the canonical writer having
   omitted every default), compare with the copy the run holds, and push only what moved through
   three new virtuals of `cue::Player`: `setEq (track, settings)`, `setFxEnabled (track, slot, on)`
   and `setFxParameter (track, slot, index, value)`. An undo moves the revision, so it is covered;
   a tick with no edit costs one integer compare. The virtuals have no-op bodies, so a Player that
   plays nothing stays a complete configuration and every fake Player in the tests stays as it is
   (plan decision 18).
4. **The audio side.** `HostPlayer` forwards to `AudioHost`, which stores: for the EQ, into the
   atomics of that voice's `CueEq`; for a plugin, into its lane of the child's shared region
   (§17.6). Relaxed stores, no lock, no allocation, no ValueTree, no message thread.
5. **The block.** The EQ reads its atomics, recomputes the coefficients of a band whose values
   moved since the last block, and runs the filters in place. The proxy, if its lane is switched
   off or its entry has failed, returns before touching the region; else it copies the block in,
   bumps the lane's parameter revision if a value moved, publishes the request, spins under the
   deadline, and copies the answer back — or passes the dry block through.
6. **The child** sees the request, applies the lane's changed values to its instance between
   blocks, processes, and publishes the answer.
7. **At arm**, the same values ride the `ArmRequest` as plain values — `request.eq`, `request.fx`
   — and are applied on the message thread in `serviceArms` and **snapped** while the voice is
   silent, as the routing is (`HostPlayer.cpp:94-116`); the lane's `resetSeq` is bumped so the
   child resets its instance and the previous cue's reverb tail does not open the next cue.

**Resting values.** Every EQ gain is 0 dB, the filters off and `eqOn` true, so a cue nobody touched
is flat by construction and `eq.reset` returns it there. A plugin parameter the cue's `values` do
not name rests at the set entry's **baseline** — the value the instance holds after the entry's
preset was applied at open, the factory default with no preset — and the published `default` under
`param/<n>` is that baseline once the child has reported it. The CSV `panic` column says `park`
throughout and is metadata still: Phase 10 applies it.

**Replay** runs handlers and no hooks (§12.1), and every write above is a handler: the EQ rows
and `values` are in the log as `node.set` records, the arms carry them, and `plugin.failed` marks
where a child died. What a replay cannot reproduce is the sound of a plugin that is not on the
replaying machine, which is the same truth `run.failed mediaMissing` already tells about a file.

### 17.5 The EQ

`audio/CueEq.{h,cpp}` names no Tracktion or JUCE type, as `CueMatrix` does not, so its arithmetic
is checked in microseconds against known responses rather than in the seconds an engine takes to
build. Six second-order sections per channel: the high-pass and the low-pass, second-order
Butterworth, and four bands from the RBJ cookbook — band one a peak or a low shelf, band four a
peak or a high shelf, two and three peaks (plan decision 6) — transposed direct form II, double
state, float in and out. Every number is one `std::atomic<float>`, every flag and shape one
`std::atomic<int>`, and each band carries a revision the setter bumps; `process` recomputes only
the bands whose revision moved, at the block boundary, with no crossfade (plan decision 7): a
rotary at 50 Hz makes small steps, and a one-block ramp is the fix if a large step ever clicks.
There was no per-band enable in 9a — a peak at 0 dB is out, and `eqOn` is the one bypass (plan
decision 5). **Amended 2026-09-25:** each band gained its own switch, `eqB<n>On`, because the author
asked for a rotary's press to switch a band in and out, and a press that parked the gain at nought
would lose the number somebody decided. A band that is off is out exactly as a band at nought is;
its control carries an `std::atomic<int>` beside the numbers, bumped with the same revision.

**When every band is identity the block is not touched**, and that is a test rather than an
optimisation: every render driver in the tree — `first_sound.py`, `phase6_sampler.py`, whose
arithmetic is that each output sample equals the gain — runs through this stage from PR 9a.2 on,
and a flat EQ that moved one sample by one bit would fail them all. The coefficient formulas and
the magnitude response live in a header-only, std-only `audio/EqMath.h` that the desktop client
includes too, so the curve the panel draws and the sound the voice makes are one function
(`check-client-boundary.py` allows it: no JUCE, no document).

`audio/EqPlugin.{h,cpp}` wraps it as `CueOutputPlugin` wraps the matrix — `xmlTypeName
"godotCueEq"`, the width carried on the state, the `getNumOutputChannelsGivenInputs` override that
sizes the buffer, zero latency, no sidechain, no automatable parameters, `ScopedNoDenormals` around
the block — and `AudioHost::buildEdit` inserts one on every voice **before** the output stage:
the chain on a voice is EQ, then the set's proxies in `plugins/order`, then the output (plan
decision 4). `initialise` runs once, or on a rate or block-size change (`tracktion_Plugin.cpp:
503-531`), so the filter state survives the graph rebuild every arm makes and a cue still
sounding on another voice hears no click.

### 17.6 The proxy

**One child process per plugin of the set, hosting one instance per voice** (plan decision 3).
It is §3.18's unit of failure — *"a process per third-party plugin"* — so a crash takes out that
plugin on every voice and nothing else; it makes P processes rather than N × P; and a preset is
applied once per instance in one place. The N round trips of one plugin in a block are serialised
through that child's worker, which is what the parent's graph does anyway with one audio CPU
(`AudioHost.cpp:111-128`); M31 says what N × P round trips cost a block.

**The region**, one memory-mapped file per child under the application's data folder (spike 07
mapped a temp file and measured 0.9 µs; the folder is the only change), laid out once:

```
Header   magic, version, layoutHash, maxChannels, maxSamples, lanes N, maxParams K (1024)
         atomic<u32> sampleRate, blockSize, childShouldExit, childReady, childFailed
         atomic<u32> catalogueReady, latencySamples, paramCount;  float baseline[K]
Lane[i]  atomic<u64> requestSeq, responseSeq
         atomic<u32> numChannels, numSamples
         atomic<u32> enabled          — Go.dot's bypass; Tracktion's is never toggled
         atomic<u32> paramRevision    — bumped by the tick thread after a store
         atomic<u32> resetSeq         — bumped at arm; the child resets the instance
         atomic<float> params[K]      — normalised; −1 means "the baseline"
         float audio[maxChannels × maxSamples]
```

`static_assert (std::atomic<uint64_t>::is_always_lock_free)`, as spike 07. The parent publishes
with release and spins on acquire, reading the clock every sixty-four turns; the deadline is
**the smaller of 250 µs and a quarter of the block period**, and `wfg serve --proxy-deadline-us=N`
overrides it (plan decision 12). **A switched-off proxy costs nothing**: with the lane off,
`applyToBuffer` returns before it reads the region — no copy, no signal, no spin. Tracktion's own
`Plugin::isEnabled` is never changed (plan decision 13): it is a `CachedValue` on the Edit's tree,
message-thread only, and leaving it alone means the node never bypasses the proxy and never builds
a latency processor for it.

**Misses, and the failed state.** A miss passes the dry block through and bumps the lane's miss
count. The **proxy host**, on the message thread on a 10 ms timer as `HostPlayer` is, reads the
counts and asks the child process whether it is running: **eight consecutive misses on any lane,
or a dead child, marks the whole entry failed** — every lane's call switched off so the proxy
stops calling, `state = failed`, `problem` a sentence, and one `plugin.failed` record submitted.
So the cost of a failure is bounded: eight deadlines once, and nothing after — spike 07's
requirement, met. The host relaunches the child **once, automatically, after two seconds**, with
the preset re-applied and every sounding run's values re-armed; a second failure inside a minute
stays failed until `plugin.restart` (plan decision 12). Spike 07's third question — whether the
spin should yield on a failed strip — does not arise: a failed strip is not called.

**The child.** The same binary re-invoked as `wfg plugin-host …` through `juce::ChildProcess`,
dispatched at the top of `runConsole` beside Tracktion's own scan child. It runs a JUCE message
loop on its main thread, because a VST3 needs one to initialise and to take state; it creates its
N instances there through `juce::AudioPluginFormatManager` with the description the parent passes
it, applies the preset, reads every parameter back as the **baseline**, reports `latencySamples`,
`paramCount` and — once per identifier — the catalogue; then a worker thread at real-time priority
(spatcore's `rt/RtThreadPriority.h`, already vendored) polls the lanes: on a request it applies
the lane's changed values to that instance, resets it if asked, processes the block, and answers.
The voice child never opens an editor - a plugin's own window is a separate helper's since the
author's redesign of 2026-09-25 (§17.13); `JUCE_MODAL_LOOPS_PERMITTED` is 0 in the child as in the parent; a
plugin that insists on a window at load, or refuses the channel count, is a `failed` with a
sentence, not a hang — five seconds for `childReady`, and then the host gives up. The child exits
when the parent sets `childShouldExit`, or when the parent's process disappears, which it checks
once a second, so a crashed parent leaves no orphan behind. **The worker's spin is a stated cost**
(plan decision 14): it spins hot while the entry is healthy and any lane is switched in — one
core per plugin of the set — and sleeps in one-millisecond polls while every lane is off or the
entry has failed; M31 records what the first request after a sleep costs, and whether a worker
that spins only between a block's first and last request is worth having.

**The test child.** CI has no plugins, and `JUCE_MODULES_ONLY` is on, so no test VST3 can be
built. The reserved identifier `godot:test-gain` makes the child skip the format manager and do
what spike 07's child did: `p0` is a linear gain with a baseline of 0.5, and `p1` at 1.0 makes the
child abort — which is how a driver kills a plugin mid-show with no Task Manager (plan decision
15). Its catalogue is two parameters, written by hand. So the whole of `/godot/fx`,
`/godot/plugin/param`, the door, the push and the failed state run in CI on all three platforms; a
real VST3 runs on the author's machine, as M31 and M34.

**Latency, honestly.** The proxy declares zero and is never Tracktion-bypassed, so no delay line is
ever built for it. A plugin's own latency — a look-ahead limiter, a linear-phase EQ — arrives
uncompensated in the child's answer, PDC being off, so a cue through such a plugin is late by that
much, on that voice, while it is switched in. `latencySamples` says how much, and the Plugins tab
shows it the moment somebody adds the plugin — §3.18's *"never discovered on the night"*. The
uniform lateness §3.18 described for a bypassed stack — every voice late by the sum of the stack's
latencies — belongs to a stack of inline plugins that declare latency while bypassed, and inline is
not built here.

### 17.7 The scan and the catalogue

**Scanning is out of process, always, and it is a verb** (plan decision 16). `wfg plugins --scan`
stands an engine up on the application's storage — the folder `AudioHost` is handed
(`AudioHost.cpp:136-149`) — and uses Tracktion's own machinery: `PluginManager::
startChildProcessPluginScan` dispatched at the top of `runConsole`, `PluginScanHelpers`' scanner
launching the current executable, the results persisted by Tracktion where it keeps them.
**Machine state, never the bundle**: which plugins this machine has is a fact about the machine,
as its MIDI device identifiers are (§15.8). `wfg plugins --list` prints it, and with nothing scanned
says so and exits 0. `JUCE_PLUGINHOST_VST3` is compiled on every platform and `JUCE_PLUGINHOST_AU`
on macOS, in the one place `WfgOptions.cmake` reserved; LV2 is Phase 9b's.

**The catalogue** is what an instance knows and a description does not: for each parameter its
name, its short name (`getName (7)`), its label, its default, whether it is discrete and its step
texts, a **bipolar** guess — a default at the middle whose text at nought begins with a minus and
whose text at one does not (plan decision 11) — and a **table of its value text at a hundred and
one points** (plan decision 10), exact for a stepped parameter. The child reports it once per
identifier and the parent caches it at `<storage>/plugins/catalogue/<sha1 (identifier)>.json`;
`wfg plugins --catalogue=<identifier>` builds one on demand. That cache is why §8's fourth item
holds without an instance — a page can label a cue that is not sounding, and even show its value
text — and why `t<n>` is published with no round trip. The `godot:test-gain` catalogue is built in.

A catalogue arriving, or a child reporting a baseline, **marks the tree stale**, or the first
snapshot would publish `p<n>` from an empty table for ever — §15.8's trap, met before.

**Amended 2026-09-26 — the list is Go.dot's own file, and every serve reads it.** The scan's
results no longer live in Tracktion's `Settings.xml`: that file is written whole two seconds after
any of its keys changes, a running serve changes several (the device setup, the wave devices), and
it holds the plugin list it read at start — so a scan beside a running show would be written over
by the next save of a key that has nothing to do with plugins. The list is
`<storage>/plugins/known.xml`, JUCE's own `KNOWNPLUGINS` form (the plugins and the skipped files),
**written only by the scan**, whole, by replacement; everything else reads it with a plain
`KnownPluginList` and no engine. A machine that scanned before the file existed has its list
imported from `Settings.xml` on the first read. Serve reads it at start **whatever its audio is**
(it had been read off the hosted engine alone, so a show on a real interface offered nothing in its
Plugins tab) into a list with a revision the tree compares at every publish, and the children make
their plugins from the same list. `/godot/plugin/known/<n>/format` is the show's word — `VST3`, `AU`
or `LV2` — where JUCE says `AudioUnit`, and `plugin.create` refuses any other word `bad-value`, since
the schema allows only those three. `serve` and `plugins` take `--engine-folder=<dir>` so a test
never reads or scans into the developer's real list. Two corrections to the text above: the
catalogue file is named by the first 32 hex digits of the identifier's SHA-256, not its SHA-1; and
`JUCE_PLUGINHOST_AU` was not compiled in 9a (§17.12) — see §17.15.

**Amended 2026-09-26 — LV2 on every platform.** `JUCE_PLUGINHOST_LV2=1` everywhere; the SDK is JUCE's
vendored copy, so no system package. An LV2 is named by a URI, not a file, and a child can only make
one whose bundle its LV2 world has loaded — the default folders and `LV2_PATH`, never a folder a scan
was pointed at with `--path` — so the scan records each LV2's **bundle folder** as a `bundle`
attribute on its element in known.xml, and the description a child is handed carries it; the child
loads that bundle before making the plugin. Each child registers **only the format of the plugin it
hosts**, since JUCE's LV2 format reads every bundle on the default folders the moment it is made. The
scan de-duplicates JUCE's answer (a bundle of two plugins is named twice). An in-tree test bundle
(`tests/fixtures/lv2/`, a stereo gain and a mono-to-stereo widener in plain C) gives every CI runner
a real LV2 to scan and host.

**Amended 2026-09-26 — the app scans (the author's decision: a Scan button in Show settings, Plugins).**
Scanning is no longer only a verb (plan decision 16 is overturned). `plugin.scan [format:s] [folder:s]`
— every format, or `vst3` / `au` / `lv2`, and a folder to search beside the format's own — and
`plugin.scanRetry <file:s>`, one skipped file scanned alone, taken off the skip list. Both are refused
`locked` while the show is locked, `scan-running` while a scan is under way, `bad-value` for a word
that is not a format. **That a scan is under way is the commands' own state**: `plugin.scan` begins it
and `plugin.scanned <found:i> <skipped:i> <problem:s>` — submitted by the engine when the scan's child
has gone — ends it, so a replay, which launches nothing, refuses exactly where the session did. The
scan is the command line's own, run as a child through the inheritance-free launcher, never inside
serve: Tracktion's scan coordinator launches its workers with handle inheritance on (serve's sockets
would go with them — the §17.12 hang), sets a process-wide environment variable, and waits in a message
loop of its own. The child reports through two files — a progress file it rewrites whole after every
plugin (`wfg plugins --progress=<file>`, with `--stop-file=<file>` and `--retry=<file>` beside it) and
known.xml — and serve reads the list again when the child has gone, asks every entry of the set that
read `missing` to start again, and submits `plugin.scanned`. Published, hand-built as the known list
is: `/godot/plugin/scan/{state,format,file,done,total,found,skipped,problem}` on the runtime half
(`state` is `idle | scanning | finished | failed`), and `/godot/plugin/skipped/<n>`, the files every
scan skips until one is retried. **LV2_PATH is set aside on Windows**: JUCE reads it with every `:`
made a `;`, which cuts a Windows path at its drive letter, and a path written the Windows way then
crashes the LV2 host outright — in the scan and in every engine start. The process forgets it before
anything reads it, says so once on stderr, and an LV2 folder that is not a default one is scanned by
name instead.

**Amended 2026-09-26 — Load now, and the graph's own slots.** The graph is fixed when it is built
(§3.25) and the set can change after it: an entry added since has no slot and no child, one taken out
or moved ahead of another has moved every slot after it. Plan decision 20 promised a sentence and
there was none; and `Runner::fxOf` numbered a cue's inserts by the set's order **as it stood now**, so
a set edited mid-session sent one plugin's switch and values to another. The plugin table now keeps
**the entries the graph was built with, in slot order** (the audio host writes it when it builds and
clears it when it stops); a cue's inserts are sent by those slots — one setting for every slot, an
entry taken out since switched out, one added since not sent — and with no graph to ask the set's own
order is the slots, as a replay has. An entry the graph was built without reads `unloaded` with
*"added since the audio graph was built; Load now rebuilds it"*, and a new row `plugins,changed`
(`/godot/plugin/changed`, T, r) says whether the set differs from the graph. **`plugin.load`** rebuilds
the graph with the set as it stands, through `audio.apply`'s door — refused `locked`, `audio-busy`
while anything sounds, prepared runs revoked, the clock gapped until `audio.settingsReady` — on
exactly what plays now: the same interface, rate, block and patches (a failed rebuild puts the graph
it had back), or the same hosted driver opened again the same way (whose render starts again). Found
while building it and put right in the same place: the graph a show asks for was assembled twice and
the two had drifted — **on a real interface a set entry's preset reached the child as the bare name
the show stores, so no preset loaded**, and `--proxy-deadline-us` was dropped; the hosted path left
the show's channels per track at two. One function (`editSpecOf`) now describes the graph for every
path.

**Amended 2026-09-26 — AU on macOS.** `JUCE_PLUGINHOST_AU=1` on Darwin, and this build — JUCE's modules,
never its `juce_add_*` helpers — links `AudioUnit` and `CoreAudioKit` itself, which is all the "link
line the Mac mini had to check" (§17.12) turned out to be. An AU has no folder: the scan asks JUCE's AU
format for the system's components whatever path it is handed, where every other format with nowhere
to look is skipped. The show's word is `AU` (JUCE says `AudioUnit`). AUv3 is refused by JUCE with a
sentence, since the children make their plugins synchronously; AU preset files (`.aupreset`) are not
read - a cue's whole saved state does their work. On the macOS CI runner Apple's AUBandpass is scanned
by its identifier and hosted in the child, where it takes a constant away.

**Amended 2026-09-26 — a plugin's buses, honestly; region version 3.** §17.6 said a plugin that refuses
the voice's channel count is `failed` with a sentence; the code asked once for the voice's width and,
refused, switched every bus on without a word, then poured the voice's channels into the first
channels of a buffer as wide as the plugin wanted — a stereo voice on a mono-only plugin put its right
channel into the sidechain, and only as many channels came back as went in. Now **a ladder**
(`PluginLoad::chooseLayout`): the voice's width in and out, two in two out, one in two out, one in one
out (a mono voice asks one-one, one-two, two-two); each asked in the standard layout for its count and
then as plain numbered channels (an LV2 whose ports name no speaker matches nothing else), first with
every bus but the main ones off and then with the others left as they are; none taken is `failed`,
*"it takes neither the voice's N channels, stereo nor mono on its main buses"*. **The rules of the
lane** (`plugin/LaneMapping.h`): the cue's channels into the main inputs one to one, a mono cue into
every one of them; every other input fed silence; the main outputs back, as many as the lane carries,
beyond that folded (output `i` onto `i mod lane` at lane/outputs); and a cue wider than the inputs, or
one the plugin would make narrower, **passes it dry, whole** — never half wet. The shared region is
**version 3**: the header carries the main widths and the layout in words, and each lane the latency
the plugin declares after that lane's state (a state can move a look-ahead; the entry's latency is the
largest a voice reports). New rows `plugin,inputs`, `plugin,outputs`, `plugin,layout`. Two more test
children beside the gain, `godot:test-mono` and `godot:test-widen` (one in, two out: left the input
times the gain, right that at a half). Until the cue's own width reaches the lane (the next
amendment), every cue is sent at the voice's width, so a mono-only plugin on stereo voices plays dry.

**Amended 2026-09-26 — a plugin can make a mono cue stereo (the author's decision).** A cue enters its
voice as wide as its file; every insert it switches in, in the set's order, either **takes it** — the
cue no wider than the plugin's main inputs, its outputs at least as wide as the cue — or passes it dry,
whole, with a sentence (`fx,problem`). A cue that takes an insert comes out as wide as the plugin's
outputs (never wider than the voice) **when it filled the plugin's inputs** — all of them, or, mono, fed
into every one; otherwise as wide as it went in, so a stereo cue in the first two inputs of a plugin as
wide as an eight-channel voice stays stereo rather than being folded at a quarter wherever it is sent.
An insert whose plugin has not said what it takes counts as taking the cue at its width, which is what
a replay and a voice with no child play. The rule is `cue/InsertChain.h` (`chainOf`, std-only), read the
same way by the runner and the tree. **The routing reads the width after the inserts**
(`media,chainChannels`): a direct out or a send to a destination with room for every side takes them
side to channel; to a narrower one, side `i` onto channel `i mod width` at width/sides — two sides onto a
mono speaker at a half each; a written Route or Feed with fewer rows than sides has side `i` take row
`i mod rows` at rows/sides; as many rows as sides, as written; a cue its inserts leave at its file's
width routes exactly as before. At the arm each insert is told the cue's width there — what is sent and
what is taken back (`FxSetting` feed/back, `Player::setFxShape`, `ProxyLane::setShape`) — and a sounding
cue follows when an insert is switched in or out or a plugin comes up (the routing and the inserts are
re-run on the plugin table's revision as well as the show's). **A widening insert that does not answer**
— late, failed — leaves a dry block whose mono side is repeated across the channels it would have
given back, so the cue plays on both sides exactly as it would with the insert out. `media,insertLatency`
sums what the inserts that take the cue declare, uncompensated; the FX panel says *"Plays as stereo
through its inserts, 21 ms late through its inserts."* and a box whose insert plays the cue dry says
why. A block longer than the region is sent in pieces under one deadline, no longer cut short. Found
beside it: a mono bus last in a saved show was built no output (its width, one, is left out by the
canonical writer and was read raw as nought).

### 17.8 The client

The desktop client keeps its rules: one snapshot a pass, `model/` std-only, one call site per
read door, no `childrenOf`, `Engine::submit` with origin `window`, every gesture a named command
pinned against the real registry (§14.16).

**The EQ panel** (`model/Eq`, `ui/EqPanelComponent`) is a subject of the foot beside the curve
editor: the response drawn on a logarithmic axis from 20 Hz to 20 kHz over ±24 dB through
`audio/EqMath.h` — the DSP's own function, so what is drawn is what is heard — with a handle per
band the mouse or a finger drags (frequency across, gain up, width on the wheel), two handles for
the filters, a shape menu on bands one and four, the on/off flags, the number always drawn beside
every handle (§4.8), and a *Flat* button that sends `eq.reset`. *Since 2026-09-25, at the
author's direction:* each handle has a colour of its own - spatcore's, red at the high-pass to
purple at the low-pass - and the one being edited is ringed and keeps the wheel and the pinch
after the hand lets go; a **pinch** sets the width, and closing the fingers narrows the band on
every road a pinch takes (two fingers on a touch screen, a trackpad's magnify, a Windows
touchpad's pinch, which arrives as the wheel with ctrl). A drag sends `node.set` throttled as the send
mixer does, and the engine coalesces. The nineteen rows leave the generic inspector's media list by
prefix, the panel being their editor.

**The FX panel** (`model/Fx`, `ui/FxPanelComponent`) reads as the send mixer does — **a box per
plugin of the set**, not per `Fx` the cue happens to hold — each with its name, its state word and
problem in words, and a switch that sends `fx.create` on the first press and `node.set enabled`
afterwards (the mixer's create-on-first-move). *Redesigned by the author on 2026-09-25 (§17.13):*
the boxes run left to right as the signal does - the EQ first, then the set, then out - and in
place of the slider-per-parameter list first drawn here, each plugin's box has *Edit…*, which
opens **the plugin's own window** in a helper process that follows the pick.

**The Plugins tab** joins Audio, Network, MIDI and Surfaces in the show settings: this machine's
known plugins from `/godot/plugin/known/<n>/…` with *Add to set*, the set with *Remove*, *Restart*
for a failed one, *Preset file…* (a native dialog, a copy into `plugins/`, one `node.set`), and
the latency and the state word beside each. With nothing scanned it says *run `wfg plugins --scan`
to see this machine's plugins* — scanning stays a verb this phase.

*Since 2026-09-26, at the author's direction (a Scan button in the app):* the tab scans. **Scan**
opens a menu of the formats (every format, VST3, AU where there is such a thing, LV2) and sends
`plugin.scan`; **Scan a folder…** sends it with a folder chosen in a native dialog. Under the
machine's list the scan says where it is in words — *Scanning 12 of 140 - Verb.vst3*, *The scan
failed: …*, *N plugin(s) known to this machine*, and with nothing known *Nothing scanned yet: press
Scan.* (`model::scanWords`). The files a scan gave up on are listed below, when there are any, each
with **Retry** (`plugin.scanRetry`). **Load now** (`plugin.load`) is lit when `/godot/plugin/changed`
says the set differs from the audio graph, and its tooltip says the sound stops for a moment and
that it waits for nothing to be playing. Scan, the folder, Retry, Add and Load now are refused while
the show is locked, and the tooltip says which of the two things - the lock, a scan already running
- stops them. Gestures `scanPlugins`, `retryScan`, `loadPlugins`, pinned in `ClientTests.cpp`.

**The page** gains the rows and nothing else (plan decision 19): its generic inspector already
writes any writable node with `node.set`, so §8's second item is free there; the nineteen names
join `KIND_ORDER.media`; an EQ curve on the page is the tablet phase's to decide.

### 17.9 Fixtures, drivers, and what the phase has to measure

**Fixtures.** `tests/fixtures/bundles/eq/` — a media cue with a shaped EQ, no audio (every fixture
is a document fixture; drivers generate their WAVs in a copy). `tests/fixtures/bundles/fx/` — a set
of one, `godot:test-gain`, and a media cue that switches it in. Replay logs, hand-written and run
under both locales: `eq.wfglog` (a create, a band written, `eq.reset`, `undo`), `plugins.wfglog`
(`plugin.create`, a rename, a delete, `undo`), `fx.wfglog` (`fx.create`, three writes to `p0`, the
switch off, `undo`) — each "reproduced exactly", the `f:` values passing through the number
formatter.

**Drivers**, registered as `blackbox.phase9a-eq` and `blackbox.phase9a-fx` under both locales:
`phase9a_eq.py` renders a two-tone file through a cue with a −20 dB peak on one tone and measures
each tone with a Goertzel filter (standard library only): the shaped tone down twenty decibels,
the other within half a decibel of flat, and a `node.set` mid-play heard in the tail.
`phase9a_fx.py` renders a constant through the test gain — half while the cue plays, unity after
`p0` is written to one, and then `p1` written to one: the child dies, `plugin/state` reads `failed`
within a second with a sentence, the render is dry from there, and the session replays.

**Measurements.** Phase 6's numbering ended at M29. Instruments that print, none a gate (§14.14's
reason); the figures go here and into PRD §6.11, with the machine named, and spike 07's caveat
about a hybrid-core laptop repeated.

| | what | what it decides |
|---|---|---|
| **M30** | the EQ's block cost — every voice with every band active, against flat, against `eqOn` off, at 96 kHz and 64 frames in M3's shape — and the tree's node-count delta from nineteen rows a media cue | whether four bands a voice fit at the widest show; what the identity fast path is worth; plan decision 1 |
| **M31** | the proxy's round trip, p50, p99 and worst: the test child on the build box, a real VST3 on the author's machine, at 1, 8 and 16 voices by 1, 2 and 4 plugins; the cost of the first request after the worker slept | the deadline; §3.18's budget stated as deadline × failed strips; plan decision 14 |
| **M32** | a failed strip: misses before the threshold trips, blocks from the kill to `failed`, the block's cost before and after | the eight-miss threshold; whether the automatic restart is welcome |
| **M33** | a parameter write to its sound — `node.set p0` at a known tick, the step found in the render, M26's idiom | whether a rotary feels live; the tick rate is the lever, parked at 50 Hz on 2026-09-18 |
| **M34** | the set at load: from `buildEdit` to every child `loaded`, and the working set, for N × P instances of a real plugin | §6.11's *"bypassed stack at load"*, answered; decision AE's cost in numbers |
| **M35** | a cue's whole state loaded onto a voice (§17.13): the load's time, and whether another voice misses while it loads | decision AI's cost in numbers: how late a cue fired cold is, and whether a load disturbs the show |

**The figures, taken 2026-09-23 on the author's Windows box (Intel Core Ultra 7 255H, 16 cores,
Debug build, the box otherwise idle), through the instruments named in each row: `M30`, `M31` and
`M32` are skipped doctest cases run with `--no-skip`; `M33` and `M34` are `tests/blackbox/
m33_param_latency.py` and `m34_set_load.py`.**

- **M30 — the EQ's block cost.** Thirty-two voices of two channels at 96 kHz, sixty-four frames,
  ten thousand blocks: **168.5 µs a block with every section in (5.3 µs a voice), of a 667 µs
  block**; **3.6 µs flat** and **3.6 µs switched off** (0.11 µs a voice) — the identity fast path
  costs the same as the switch. Four bands and two filters on every voice of the widest show fit
  in a quarter of the block. The tree's delta is nineteen nodes a media cue, by construction.
- **M31 — the proxy's round trip.** A 20 ms deadline so a late answer is measured rather than
  dropped; every lane switched in and driven in turn, the last lane timed over 2,000 blocks:

  | plugin | lanes | block | p50 | p99 | max | misses | first after idle |
  |---|---|---|---|---|---|---|---|
  | test gain | 1 | 64 | 0.6 µs | 0.8 µs | 318.6 µs | 0 | 19.2 µs |
  | test gain | 8 | 64 | 0.5 µs | 0.7 µs | 5.3 µs | 0 | 106.1 µs |
  | test gain | 16 | 64 | 0.5 µs | 0.7 µs | 12.3 µs | 0 | 15.5 µs |
  | test gain | 16 | 256 | 1.1 µs | 1.3 µs | 3.9 µs | 0 | 20.7 µs |
  | WFS-DIY Track | 1 | 64 | 1.6 µs | 2.3 µs | 86.9 µs | 0 | 26.0 µs |
  | WFS-DIY Track | 8 | 64 | 1.5 µs | 2.0 µs | 7.8 µs | 0 | 26.9 µs |
  | WFS-DIY Track | 16 | 64 | 1.5 µs | 1.8 µs | 4.7 µs | 0 | 19.4 µs |
  | WFS-DIY Track | 16 | 256 | 1.8 µs | 2.1 µs | 17.6 µs | 0 | 19.0 µs |

  Spike 07's 0.9 µs, held with a real plugin; the worst outliers are the first blocks of a
  configuration. Sixteen voices through one child cost 16 × 1.5 µs a block. The first request
  after the worker's idle poll costs 15–106 µs — the one-millisecond sleep's wake — which is
  what the hot spin buys back while a lane is in (plan decision 14 stands; the spin-between-
  first-and-last variant is not needed at these numbers). **Not measured here: the same on
  a shared CI runner, where the 250 µs default misses on and off** — the fx driver saw it on both
  Linux and macOS and now serves at 20 ms, because a driver proves the path.
- **M32 — a failed strip.** Healthy at the 250 µs default: p50 0.6 µs, p99 4.3 µs, no miss in
  500. The child killed: **three blocks at the block rate, each a miss costing 252 µs (the
  deadline and nothing more), and `failed` 8.8 ms after the kill** — the dead-child check trips
  before the eighth miss would. Failed and not called: **0.00 µs p50, 1.1 µs max**. The
  automatic restart then brings the child back in two seconds (the driver shows it, and shows
  the second death staying down).
- **M33 — a parameter write to its sound.** `node.set p0` on the test gain, the render searched
  for the step against the engine's own tick: **40–61 ms, median 41 ms** over ten writes — two
  ticks: the write lands on one tick, `applyFx` runs at the next tick's start, then the block.
  A first draft read the render file's size for the moment of the write and saw the writer's
  flush cadence as a half-second on every other write; the engine's tick is the reference. If a
  rotary wants one tick rather than two, `applyFx` after the tick's command drain is the lever
  before the tick rate is (§16's parking of 50 Hz stands).
- **M34 — the set at load.** Served hosted, from the ports printed to every entry `loaded`, and
  the working set off the operating system:

  | plugin | voices | entries | ports → loaded | parent | each child |
  |---|---|---|---|---|---|
  | test gain | 1 | 1 | 705 ms | 48 MB | 32 MB |
  | test gain | 16 | 3 | 961 ms | 57 MB | 33 MB |
  | WFS-DIY Track | 1 | 1 | 759 ms | 48 MB | 36 MB |
  | WFS-DIY Track | 8 | 3 | 1,002 ms | 55 MB | 45 MB |
  | WFS-DIY Track | 16 | 1 | 1,077 ms | 58 MB | 55 MB |
  | WFS-DIY Track | 16 | 3 | 1,151 ms | 60 MB | 55 MB |

  Decision AE's cost in numbers: **about a second to load whatever the shape, and a child of
  36 MB plus 1.2 MB a voice for this plugin** — the children come up in parallel, so three
  entries cost a hundred milliseconds more than one, not three times. §6.11's *"bypassed stack
  at load"* is answered for the proxy; the inline figure is Phase 9b's.
- **M35 — a cue's whole state, loaded onto a voice** (added 2026-09-25 with §17.13). A skipped
  case in `tests/ProxyTests.cpp` (`WFG_REAL_VST3=<identifier> --no-skip`): two states made by the
  editing helper as a hand would, then a two-voice child with voice one playing a block every
  1.333 ms at the default 250 µs deadline and voice two loaded with the two states in turn, twenty
  times; then the same length with no loads. Three runs, 2026-09-25, same box:

  | plugin | state | load min | median | max | voice-one misses while loading | the same time idle |
  |---|---|---|---|---|---|---|
  | test gain | 21 bytes | 0.20–0.24 ms | 0.27–0.31 ms | 0.59–0.86 ms | 0 | 0 |
  | WFS-DIY Track | 1,012 bytes | 0.32–0.36 ms | 0.38–0.46 ms | 0.64–1.16 ms | 0 | 0 |

  Decision AI's cost in numbers, for this plugin: **a cue fired cold is late by about half a
  millisecond** - under one block at 64 frames, far under a tick - **and a load disturbs no other
  voice.** The instrument's first version counted one miss every run: voice one's first block
  finding the worker asleep (M31's *first after idle*), not the load; it now warms up before
  counting. A plugin with a large state - a sampler, a convolution reverb with its impulse inside
  - is the measurement still to take.

### 17.10 The direction this phase does not build

**The surface pages** — the focus row, the page model, the EQ page's three encoders a band, the FX
page's sixteen parameters and its arrows, the lit buttons — and **the virtual panel's rotaries**:
the pages session's, on top of this, in the order the author set (`docs/godot-surface-pages-
draft-0.1.md` §11). The addresses it needs are §17.2's, and it should have them when 9a.3 lands
rather than at close-out.

**An EQ on a bus or an output** — a system EQ for the room. Every EQ here is a cue's.

**A fade aimed at an EQ band or a plugin parameter.** A fade moves a level or a DCA; a curve on
any other parameter is §3.10's binding, Phase 10's and later. `advanceFades` is untouched.

**Inline hosting** — §3.18's opt-in, a `te::ExternalPlugin` in the process with §3.4's
message-thread handover. Not built and given no row, an option nothing reads being rot.

**LV2**, **AU presets**, **curated per-plugin parameter maps** (the
pages draft's §7.2, a machine-level library), **a page EQ curve**, and **macOS audio workgroups for
the child** — a Mac child can be scheduled late, and M31 on the Mac mini says by how much. (A
plugin's own window was on this list until the author asked for it on 2026-09-25; it is §17.13.)

**The live-input rack, rack-channel chains and the shared reverb channel** — Phase 9b, with
`Media/Insert` and `Rack/Channel` waiting for it exactly as Phase 4 left them.

**Panic values applied** — the column stays metadata until Phase 10.

### 17.11 Decisions to overrule early

Taken with the plan rather than by the author, each built on, and each cited above as *plan
decision N*:

1. **The EQ is nineteen rows on `media`**, not an `<Eq>` child: direct addresses, no create,
   sparse in the file, dense in the tree.
2. **The cue's insert is `<Fx>`, owner `fx`**; `Insert` stays Phase 4's claim; no second element
   named `Plugin`.
3. **One child process per plugin of the set, hosting one instance per voice**; failure is per
   plugin across every voice.
4. **The chain on a voice is EQ, then the set in `plugins/order`, then the output stage**; a cue's
   `Fx` children do not reorder it.
5. **The high-pass and the low-pass are flags; a peak band has no enable** — 0 dB is out; `eqOn`
   is the one bypass.
6. **Band one is `peak | lowShelf`, band four `peak | highShelf`, bands two and three peaks**;
   on a shelf, Q is its steepness.
7. **Coefficients jump at the block boundary, with no crossfade.**
8. **A cue's values are one sparse `index:value` row, writable whole, with `p<n>` as the door a
   hand uses**; the key is the parameter's index, so a plugin whose list changes between versions
   is a re-edit (a stable parameter identifier is the alternative when it bites); a machine whose
   catalogue does not know the plugin accepts any index.
9. **A value the cue does not name rests at the set entry's preset baseline**, and the published
   default is that baseline once the child reports it.
10. **Value text is a table of a hundred and one samples cached with the catalogue**, not a live
    question to an instance.
11. **`bipolar` is guessed** from the default and the texts at the ends, and published; a curated
    map can overrule it later.
12. **The deadline is the smaller of 250 µs and a quarter of the block period**, overridable on
    the command line; **eight consecutive misses or a dead child mark the entry failed; one
    automatic restart after two seconds, then `plugin.restart`.**
13. **Tracktion's `Plugin::isEnabled` is never toggled**; Go.dot's lane flag is the bypass.
14. **The child's worker spins hot while the entry is healthy and any lane is on**, one core per
    plugin, and sleep-polls otherwise; revised by M31.
15. **The test plugin is a child mode, `godot:test-gain`**, with `p1` as its kill switch; no VST3
    is built for the tests.
16. **Scanning is a verb**, `wfg plugins --scan`, not an engine command; the Plugins tab says so
    when the list is empty.
17. **A preset is a `.vstpreset` file under the bundle's `plugins/`**, named by the row and
    copied by `saveAs`, never bytes in the row.
18. **The new `Player` virtuals have no-op bodies**, not pure ones; every fake Player stays as it is.
19. **Inline is not built and has no row; the page gets rows and no curve.**
20. **A set edited while the show is open changes no graph**: the entry reads `unloaded` with the
    sentence *added since the show opened; reload to load it*, and a deleted entry's proxy stays,
    switched off.
21. **The phase is 9a**: Phase 9 keeps its number and its name and splits into 9a, built now, and
    9b, the live rack and what §17.10 leaves; no later phase is renumbered. This section is §17,
    the decisions AD–AG, the measurements M30–M34.

### 17.12 What Phase 9a built, against what §17 drew

*Written 2026-09-23, late, at the end of the session that built 9a.0–9a.9; the figures are not
in yet (below).*

**What landed on `main`, in order:** 9a.0 the docs; 9a.1 the EQ's arithmetic, pure; 9a.2 the EQ on
every voice with its nineteen rows, the arm, the push and `eq.reset`; 9a.3 the EQ drawn at the foot
from the DSP's own function; 9a.4 the plugin set as a document object; 9a.5 hosting compiled in,
the scan verb and the catalogue; 9a.6 the proxy transport; 9a.7 the child hosting a real plugin;
9a.8 the cue's inserts with the `p<n>` door; 9a.9 the inserts as the client reads them and the
Plugins tab. Every one has its unit cases, the replay fixtures `eq.wfglog`, `plugins.wfglog` and
`fx.wfglog` under both locales, and the drivers `phase9a_eq.py` and `phase9a_fx.py` at the black
box. On the author's machine a real VST3 - his own WFS-DIY Track - came up on two voices through
the sandbox, fifteen parameters catalogued, twenty blocks answered without a miss, a value taken.

**Where the build departed from the drawing, and why:**

- **`fx/plugin` names the set entry's ID, not the scan's identifier** (§17.2 said identifier). A set
  may hold one plugin twice, and a cue must say which; the identifier is on the entry.
- **`plugins/order` is a list of entry ids**, for the same reason, and `fx/index` is the entry's
  position in it.
- **The scan owns its file loop and has a deadline.** Tracktion's coordinator waits for a reply
  without a limit, and the first scan on the author's machine hung for good on a plugin whose
  licence check never returned; a file overdue by thirty seconds is skipped, blacklisted and named
  (`--retry-skipped` asks again). And a hung child is terminated by pid, because JUCE's kill is a
  message on the pipe that a process hung in plugin code never reads - the first scan left one
  behind at a gigabyte.
- **The plugin child is launched without handle inheritance on Windows.** JUCE's `ChildProcess`
  inherits every handle, and a socket is one: the first hosted serve handed its HTTP sockets to
  the child, and every client waiting for the server to close a connection waited for a process
  that never would. `ProxyHost` has its own `CreateProcessW` on Windows.
- **A failed child is put down at once**, not asked politely: the wait ran on the message thread.
- **`ProxyPlugin` lives under `audio/`**, beside `EqPlugin`, because it names Tracktion; the
  transport it wraps (`ProxyLane`, `SharedRegion.h`, `ProxyHost`) is under `plugin/` and names no
  JUCE type in its headers.
- **A state `missing` is published** for an entry this machine's scan does not know: no child is
  launched, the sentence says what to do, and every voice plays dry through the slot.
- **A `.vstpreset` goes through JUCE's VST3 client (`setPreset`)**, anything else through
  `setStateInformation`; the SDK's own loader either way.
- **The catalogue store and the plugin table carry a revision the tree compares at every
  publish** (the mount table's idiom), so a child's report on the message thread reaches a client
  without anyone marking the tree stale from a thread that must not.
- **`plugin.failed` on replay knows the set from the document** (`pluginKnownBy`), not from a table
  a replay never fills; the driver found it.
- **The bipolar guess is wrong on a shelf**: WFS-DIY's "HF Shelf" runs -24..0 dB about a -12 dB
  middle and reads as bipolar. The curated map plan decision 11 reserved is where that goes.
- **AU hosting is not compiled yet**; VST3 on every platform is. The Mac mini's link line decides.
- **The macOS child needs `initialiseNSApplication()`** before its dispatch loop, as the console's
  serve loop does; the macOS CI job found the child leaving before it answered a block.
- **The tree reads a table's revision BEFORE the table**, not after: a child that came up during
  a slow rebuild was published `loading` for the rest of the session on the Linux CI job.
- **The fx driver serves at a 20 ms deadline.** At the 250 µs default a shared CI box misses on
  and off and the render averages between dry and processed; a driver proves the path, and M31
  measures the round trip on a quiet machine.
- **The driver reads a block-wise median, not a mean.** Even at 20 ms the macOS runner had a
  quarter of the blocks late in one run of two; a median says what the processed blocks are at
  and still fails when more than half are dry. **The macOS lateness itself is not explained:**
  the child's worker takes a mach time-constraint policy and then spins hot while a lane is in,
  and macOS demotes a real-time thread that overruns its constraint - a yield or a short sleep
  between polls on macOS, or no time-constraint policy for a spinning worker, is the thing to
  try on the Mac mini with M31 in hand.
- **The child's test mode also writes its catalogue file**, so the parent's pickup runs in CI.

**What is not built, of §17's own list:** the surface pages, the virtual panel's rotaries, a system
EQ, a fade on a parameter, inline hosting and LV2 as §17.10 said. The **FX panel at the foot**,
unbuilt when this was first written, was built on 2026-09-25 in the author's redesign: the chain,
the plugin's own window and the whole state per cue (§17.13).

**The figures M30–M34 gave:** in §17.9, taken the same evening once the box was quiet. In one line
each: the EQ costs 5.3 µs a voice with every section in and nothing flat; a round trip through a
real plugin is 1.5 µs at p50 with no miss at sixteen voices; a dead child is `failed` in 9 ms after
three 252 µs misses and costs nothing after; a write reaches the sound in two ticks, 41 ms; a real
plugin's child is 36 MB plus 1.2 MB a voice and every entry loads in about a second.

**What only the author can settle:** what he sees on the desktop - the EQ panel's feel (plan
decisions 1, 5, 7), the deadline and the spin policy once M31 is in (12, 14), and whether the
automatic restart is welcome (M32).

### 17.13 The chain, the plugin's own window, and the whole state per cue (2026-09-25)

*Written at the author's direction (2026-09-25), who overrode the PRD and this section's earlier
text to ask for it: "for VST and such could we just show the chain, bypass switch and open the
native plugin UI as a popup?" - and then "Yes, I'm overriding the PRD and the rest of the
documentation." What it replaces: §17.6's "no editor is ever opened", §17.8's slider-per-parameter
FX panel, and §17.10's "any plugin editor window" as a thing not built.*

**The five decisions, asked one at a time with a recommendation beside each.**

| | the question | the author's answer | recommended? |
|---|---|---|---|
| **AH** | how the foot shows a cue's inserts | the chain, left to right, the EQ first: `file ▶ [EQ] ▶ [1. plugin] ▶ … ▶ out`; each plugin box a switch, its state in words, and *Edit…* | yes |
| **AH** | where the plugin's own window runs | a separate **editing helper**, a process of its own, never in the audio path | yes |
| **AH** | what the window does when the pick moves | it **follows the pick**, and greys when the cue has no such insert | yes |
| **AI** | what is kept per cue | **the whole state of the plugin**, not only its parameters | no - parameters only was recommended |
| **AI** | when the state is kept | automatically, a moment after the hand stops, **the turn and its state one Undo** | yes |

**AH - the chain and the window.** The foot's FX panel (`ui/FxPanelComponent`) draws the signal
path every voice carries, box by box: Go.dot's EQ with its switch (`eqOn`) and *Open*, which shows
the EQ panel in the same foot; then each entry of the set in `plugins/order` with its switch - the
first press on an entry the cue has no `Fx` for is `fx.create`, every press after is `enabled` -
its state word and sentence (`missing: … - this cue plays it dry` only when the cue has it in), its
latency in words, and *Edit…*. No sliders: the plugin draws its own controls better than a generic
list could. *Edit…* opens the plugin's **own** window in a helper process, `wfg plugin-editor`,
which loads its own copy of the plugin through the same making as the voice child (`PluginLoad`),
never processes a block for a voice, and so can crash without silencing anything. The desktop
client owns it (`ui/PluginEditors`, `plugin/EditorHost`): opening a window is not a change to the
show, so it is no command, like a file chooser; what the window *does* is ordinary - each value
the plugin's window moves becomes one `node.set` on the cue's `p<n>` with origin `window`, the
write a slider would send, coalesced into one step, followed by a sounding voice within two ticks.
A value is written to the insert of the cue it was **moved on**: every event carries the sequence
of the subject it was made under, so a turn made as the pick moves never lands on the next cue.

**The window follows the pick.** Once a pass, from the pass's own snapshot, every open helper is
handed its subject: the picked cue's title, whether the window is greyed and why (*nothing is
picked*, *2 Memo plays no file*, *Verb is not on 3 Steady: switch it in from the FX panel*), the
cue's value for every parameter, and its state file. Greyed, the editor is **hidden** and the
sentence drawn in its place - a native plugin view covers anything drawn over it - and nothing the
window does is written. *Edit…* on an insert the cue has not got switches it in first (`fx.create`)
and opens once the tree shows it, unless the pick moves first. Values moved elsewhere - an undo,
the page, a surface - come into the plugin, but never over a hand: a parameter in a gesture, or
one this window moved in the last 300 ms, keeps the hand's value until the tree has agreed.

**The show's keys come back.** The window belongs to another process, so while it is in front the
keyboard is its: Space and Esc pressed in it and not taken by the plugin are handed back to the
client's own key handling - GO, and §4.4's two stops. Best effort, and said so: a plugin view that
takes the keyboard for itself keeps it. The helper's window stays above Go.dot's only while Go.dot
or the helper is in front, and is **not owned by Go.dot's window across processes**: on Windows
that joins the two processes' input queues, and a plugin window that hung would freeze the GO
button - the one thing a separate process exists to prevent. The lock closes every helper and
*Edit…* says why.

**AI - the whole state per cue.** What a plugin's window changes that is not a parameter - an
impulse response, a sample, a mode - is kept with the cue as a file:

- **`fx/stateFile`** (new row, `s`, rw, persist show): a name under the bundle's `plugins/` -
  `state/<entry id>-<first 16 hex of the bytes' SHA-256>.state` - or empty for the entry's preset.
  Files are **content-addressed and never changed or deleted by Go.dot**: the same state is always
  the same file, and Undo only points the row back at an older one. Writing the bytes is not a
  change to the show (a fact about the disk, as a media file copied in is); naming them is.
- **`fx.capture <fx> <stateFile> <values>`** (new command): the file's name and **every**
  parameter's value, in one transaction - so a cue with a state carries its whole parameter
  picture, and "a value the cue does not mention rests at the preset" never fights the state's own
  values. It never touches the disk (the tick thread; a replay has no files); it refuses a name
  that is not `state/<name>.state` in letters, digits, hyphens and underscores; the lock refuses it.
- **When the helper captures:** only when a hand changed something - a parameter, or the plugin
  saying its state changed - and the bytes' hash differs from what the plugin held when last
  loaded or kept; then **1.5 s after the last change** with no gesture open, when the window
  closes, before the window moves to another cue (kept under the OLD insert), and on the way out.
  One silent block is processed first, because a VST3 learns of its editor's turns in its
  processor only at the next block. Never while greyed, never for values the cue sent it, never
  for a show with no folder (the chain says *save the show to keep its whole state*).
- **The turn and its state are one Undo.** `ShowDocument::beginTransaction` gained one rule: an
  `fx.capture` on insert F **joins the open transaction** when the last write was to one of F's
  `p<n>`, from the same origin, within **125 ticks** (2.5 s). A capture after anything else - an
  impulse response loaded, which moves no parameter - is a step of its own; a second capture never
  joins; the turn after a capture is a new step. Keyed on logged ticks and origins, so a replay
  splits exactly as the session did.
- **Loaded on the voice at the arm, before the cue may launch.** The arm hands the lane its cue's
  state path (`ProxyLane::wantState`, the shared region's lane at version 2: `stateRequestSeq`,
  `stateDoneSeq`, `stateFailed`, `stateLoadMicros`, `statePath`, `stateProblem`). The voice child
  loads it on its message thread - where a VST3 takes its state - with the lane **parked**: its
  real-time worker lets go of that instance and answers its blocks **dry**, so no block is ever
  missed, while the other voices play on; then every value is set again on top, and the parent is
  answered with how long it took and why it could not. **No state is a state:** a cue with none,
  arming on a voice that last held another cue's, is given the preset's own state back, or that
  cue would be heard under this one. **The wait is `HostPlayer::isArmReady`**, which now asks
  whether every switched-in entry of the voice holds its state (two atomics a lane on the tick
  thread): a cue armed in standby is always ready; a cue fired cold is **late by the load, not
  wrong**, and `run.late` says by how much. A state changed after the arm and before the launch
  (an undo in standby) is loaded before the launch; one changed on a cue already sounding is not -
  the knobs follow live, the rest applies next time the cue plays. A file that is not there is
  the preset with a sentence (`plugin/stateProblem`), and `wfg validate` names every cue whose
  state is missing from the bundle, or outside `plugins/`. A child that has been loading one for
  five seconds is hung and failed like any other; the state that was loading when a child died is
  not sent again.
- **What the entry says:** `plugin/stateLoadMs` (how long the last state took) and
  `plugin/stateProblem` (why the last could not load), machine rows, beside `latencySamples`.

**The honest cost of AI, stated as it was chosen.** M35 (§17.9) puts the load at about half a
millisecond for the author's own plugin, with no miss on any other voice while it loads. Every
arm whose voice holds another state is a
load, including a return to "no state"; a scene of N cues with states loads them one after
another; a state that embeds a sample's path does not travel between machines; the bundle grows
by one file a burst of editing, with no tidy command yet; a plugin that keeps window details (a
tab, a size) in its state captures on window-only actions. And a plugin whose `setState` takes a
lock its `process()` also takes on other instances would make other voices miss while it loads -
M35 is the measurement that says whether the author's own plugins do.

**Defaults to overturn once seen working:** the 1.5 s quiet moment; the 125-tick join; the 300 ms a
hand's value wins; topmost only while Go.dot is in front; greying by hiding the editor; Space and
Esc as the only keys handed back; five seconds before a loading child is called hung.

**Built** (2026-09-25, four commits on `main`): the chain panel; the editing helper and its region
(`plugin/EditorRegion.h`, `EditorHost`, `PluginEditorChild`, the test gain as a real processor with
a Pad switch that is state and not a parameter); `ChildLaunch` and `PluginLoad` factored out of the
proxy so both children start and make a plugin the same way; `cue/FxValues` moved out of `FxRows`
for the std-only client model; `fx/stateFile`, `plugin/stateLoadMs`, `plugin/stateProblem`,
`fx.capture` and the join rule; the voice child's gate, epoch and state loader; the arm's wait;
`wfg validate`'s two sentences. Tested by `EditorHelperTests`, the proxy's state case, `FxRowsTests`'
capture case, the UI binary's chain and plugin-window cases, and `blackbox/phase9a_fx.py`'s second
session, which hears it: Pad in a cue's state and the cue plays at a quarter of a half; Undo, fire
it again, and it plays at a half.

**Found on the way, not this section's to fix:** on a one-track show, a cue fired again within
about 50 ms of `run.killAll` plays silence while its run reports `playing` - found by the fx
driver with no plugin at all; at 300 ms it plays. The driver waits for the killed run to finish.

### 17.14 The EQ and Send pages, and a locked show ridden live (2026-09-25)

*Written at the author's direction (2026-09-25, evening): "For the rotary dials when pressing on the
EQ button while a sample is selected (select button) assign rotaries to the EQ ... Same for Send
levels ... Pressing the Star key exits from the EQ and Send modes ... These edits can happen even if
the samples are not playing." The page model of `docs/godot-surface-pages-draft-0.1.md` §5, and its
§9 questions 1, 3 and 5, are answered here.*

**The seven decisions, asked with a recommendation beside each.**

| | the question | the author's answer | recommended? |
|---|---|---|---|
| **AJ** | what aims the rotaries | SELECT on a sample strip, or a click on a running cue's name in the window - and not the cue list's pick; while a page is up the foot shows the surface's cue | yes |
| **AK** | how a press switches a band or a send | a new saved switch each (`eqB<n>On`, `send/on`): off keeps the number | yes |
| **AL** | what SELECT's light says | the pick - the D700's white bar - and no longer that the strip sounds | yes |
| **AM** | the pages under the show lock | **EQ and sends ride live, unsaved**, like a fader's trim | no - closing the pages under the lock was recommended |
| **AN** | how long a live change lasts | until the show is unlocked, and then a bar asks: Keep (one undo step) or Discard | yes |
| **AO** | whether the window rides live too | yes - the EQ panel and the send mixer, and the page | yes |
| **AP** | a send the cue did not have, turned up under the lock | **made live too**, and Keep makes it a real Send | no - existing sends only was recommended |

**The rows.**

| Node | Type, default | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/cue/<id>/eqB{1..4}On` | `T`, true | rw | show | a band's switch (AK); §17.2's table carries it |
| `/godot/send/<id>/on` | `T`, true | rw | show | a send's switch (AK) |
| `/godot/surface/aim` | `s` | ro | none | the cue every surface's rotaries edit (AJ); §16.2 |
| `/godot/surface/<id>/page`, `pageIndex`, `pageCount`, `edited` | | ro | none | what a surface's rotaries show; §16.2 |
| `/godot/cue/<id>/live` | `s` | ro | none | the names of this cue's EQ rows a locked show is riding live |
| `/godot/cue/<id>/sends` | `s` | ro | none | this cue's sends by identifier: its Send children, then the ones made live |
| `/godot/send/<id>/live` | `T`, false | ro | none | whether this send rides live - a value held, or the whole send made under the lock |
| `/godot/document/live` | `i`, 0 | ro | none | how many changes ride live: what the window's bar counts |
| `/godot/audio/mixes` | `s` | ro | none | the show's mix channels in output order - first channel, then identifier, the send mixer's own order - one a Send page's rotary |

**The commands.**

| Command | Arguments | What it does | What it refuses |
|---|---|---|---|
| `surface.aim` | `<cue s>` | §16.3 | a cue that is not media, `bad-value`; none, `unknown-id` |
| `live.keep` | — | writes every live value into the show, one transaction; a send made live becomes a Send with the identifier it rode under, or gives its values to a send the show has since been given into that bus; a cue or bus deleted since is skipped | under the lock, `locked` |
| `live.drop` | — | lets go of every live value, and gives back the identifiers the live sends had reserved | nothing |

**The doors.** Under the lock, `node.set` on a cue's EQ row or a send's `level` or `on` is answered
by `cue::liveEditFor`, composed into the `node.set` door in serve and in replay between the trim
door and the FX door. It resolves and parses exactly as the document's door would, so it refuses
what the document would refuse, and holds the value in `cue::LiveEdits` instead of refusing it as
`locked`. A value the show already has is no change: it drops whatever rode there. `send.create`
asks `cue::liveSendFor` first (a new `LiveCreate` hook on `registerDocumentCommands`): under the
lock it makes the send live, the cue media and the bus a mix channel, one send per bus across the
show and the layer, its identifier drawn and reserved and carried on the applied record.
`eq.reset` under the lock holds flat live, row by row where the show differs from flat. Once the
show is unlocked, a write to a row that rides live writes the show - an undo step - and drops that
live value; a send made live keeps riding until Keep or Discard, since a `node.set` cannot make an
object. The transaction hook asks `cue::isLiveEdit` beside `isLiveWrite`: a ride opens no
transaction.

**Who reads the layer.** `Runner::eqOf` and `resolveRouting` ask it before the show, so an arm
carries what is heard and a sounding cue follows on the next tick: `applyEq` and `applyRouting` are
gated on the layer's revision as well as the show's. The tree publishes the value heard at the
saved value's address - one address, one value, whoever reads it - and rebuilds its document half
when the layer's revision moves.

**What it costs, said as it was chosen.** A change made live is heard and not saved: a crash, a
close, or `--recover` loses it, as it loses a fader's trim. An undo taken after unlocking, with
live values still riding, can appear to do nothing where the layer covers the row it restored; the
bar is the answer. A live send's identifier is reserved in the registry, which a rebuild after an
undo forgets; were another object to draw the same eight characters before Keep, that send would
be skipped rather than made twice.

**Built** (2026-09-25, on `main`, in the plan's stages): the switches; `surface.aim` and the page
rows; SELECT and the EQ page on the bridge (`surface/SurfacePages`, the band colours from one header,
`audio/EqColours.h`); the live layer (`cue/LiveEdits`, the two doors, `live.keep` and `live.drop`,
the Runner and tree overlays, `tests/fixtures/logs/live.wfglog` replayed in both locales); the Send
page (`/godot/audio/mixes`, a send created by a turn up from silence or a press, at nought); the
window - the foot held on the surface's cue while its page adjusts it and handed back when the page
closes, the band last turned ringed, the running pane's name aiming the rotaries with a mark,
the band switches in the EQ panel and the send switches in the mixer, "live" said on both, and
the Keep / Discard bar under the transport (`ui/LiveBarComponent`); and the page - the same bar's
two buttons, a media cue's sends listed and inspected like its triggers, and `+` for a mix channel
it does not reach. **Owed to the bench:** the port the master section's lights answer on, the
detent laws, the colours at a glance - `tests/blackbox/make_d700_bench.py` walks them.

### 17.15 Plugin hosting finished: the scan in the app, AU and LV2, and how an insert's channels flow (2026-09-26)

Asked by the author on 2026-09-26 — *"Should we add or finalise plugins VST/AU/LV2 integration?
Scanning and audio patch for the media cue inserts"* — and answered with three decisions, each the
recommendation offered (AskUserQuestion). The letters go on from §17.14's AP, and skip **AU** so a
decision letter never reads as the plugin format.

| | Decision | Recommended? |
|---|---|---|
| **AQ** | **A Scan button in the app**, Show settings → Plugins: the command line's out-of-process scan, launched as a child; progress in words; skipped files listed with Retry; never automatic; refused while locked | yes — the author's |
| **AR** | The machine's list is `<engine>/plugins/known.xml`, written only by the scan, read in every mode (§17.7) | implementer's call, from the code |
| **AS** | **AU on macOS and LV2 everywhere**; AUv3 and `.aupreset` out | yes — the author's |
| **AT** | **A plugin can make a mono cue stereo**: mono fed into every input, both sides kept; the routing reads the width after the inserts; summed where there is room for one | yes — the author's |
| **AV** | The layout ladder: the voice's width, 2-2, 1-2, 1-1, each standard and numbered; never every bus on; a sidechain fed silence | implementer's call |
| **AW** | A cue wider than an insert takes, or one it would make narrower, passes it dry, whole, with a sentence — never half wet | implementer's call |
| **AX** | **Load now** (`plugin.load`): the graph rebuilt with the set as it stands, on what plays now, only while nothing plays | implementer's call |
| **AY** | Latency shown — per entry, per cue through its inserts, re-read after each state — and still never compensated | implementer's call |

**Built** (on `main`, one commit a stage): the known list in every mode and `--engine-folder` (stage 1);
LV2, the in-tree test bundle, a child registering its plugin's format alone, the bundle recorded
beside each LV2 (2); the scan from serve — `plugin.scan [format] [folder]`, `plugin.scanRetry`,
`plugin.scanned`, `/godot/plugin/scan/…`, `/godot/plugin/skipped/<n>` — and LV2_PATH set aside on
Windows (3); Load now, `/godot/plugin/changed`, and a cue's inserts sent by the graph's slots (4); the
Plugins tab's Scan, folder, Retry and Load now (5); AU on macOS (6); the layout ladder, the lane's rules,
region version 3, `plugin,inputs|outputs|layout`, and the test children `godot:test-mono` and
`godot:test-widen` (7); the chain's width (`cue/InsertChain`), the routing reading it,
`media,chainChannels|insertLatency`, `fx,problem`, the FX panel's words, a widening insert's dry block
widened, blocks sent in pieces (8).

**What the code turned up on the way**, each put right where it was found and said in its stage's
amendment above: a device session handed the proxies before the catalogue store was set, so children on
a real interface reported their catalogues into nothing; a set edited mid-session sent a cue's
settings to the wrong plugin; on a real interface a preset reached the child as a bare name (so no
preset loaded) and `--proxy-deadline-us` was dropped; hosted voices ignored the show's channels per
track; a mono bus last in a saved show was built no output; `wfg plugins --scan` had never scanned
anything on macOS - its message loop is `[NSApp run]` and returned at once without the application,
stopping the scan before its first file (9a.7's trap, met again); the audio host cleared a plugin
table at its destructor that a test had already destroyed (macOS aborts on the dead mutex); and a set
`LV2_PATH` crashed JUCE's LV2 host on Windows.

**Owed to the bench and the Mac mini:** a real scan of the author's plugin folder from the app, and
M37 (a scan during a rehearsal); a mono cue through a real stereo reverb on the MADIface, stereo on a
stereo out and summed on a mono one, and whether switching it in mid-cue bumps; a mono-only plugin
saying "plays dry"; the latency words against a look-ahead plugin; how long Load now's gap is; M36
(LV2 at a session's start); and on the Mac mini a third-party AUv2 from scan to sound, its window and
a state round trip, M31 there, and the AUv3 sentence read by a person.

### 17.16 The FX page: an insert's parameters on the rotaries (2026-09-26)

Asked by the author on 2026-09-26: *"When pressing the FX button on the MIDI controller (Mackie mode
or D700) can the selected effect on the selected media cue (and future fx processing chain in the
Effects rack) be assigned to the rotaries in a similar fashion to the ones for the EQ and Sends?"*
Four decisions, each the recommendation offered (AskUserQuestion), and three implementer's calls.
The letters go on from §17.15's AY. The pages draft's §7.2 and its §9 question 9 are answered here.

| | Decision | Recommended? |
|---|---|---|
| **AZ** | **The plugin's own parameter order**: the first sixteen on a two-unit D700, eight on one unit or a Mackie surface; FX pressed again pages through the rest. Curated per-plugin maps later (Phase 9b) | yes — the author's |
| **BA** | **FX walks the inserts in chain order**: insert 1's pages, then insert 2's, then back to the surface's own page. The first rotary's third row says which ("Verb 1/2", or the name alone for a one-page insert) | yes — the author's |
| **BB** | **Under the lock a parameter rides live**, as EQ and sends do (AM): heard on the next tick, written to nothing, no step of the history; Keep or Discard once unlocked | yes — the author's |
| **BC** | **The encoder's press puts that parameter back to its default**: the plugin's own, as the catalogue read it. Switching an insert in and out stays in the window | yes — the author's |
| **BD** | The page walks the cue's **switched-in** inserts (`/godot/cue/<id>/fx`), not every entry of the set: a turn never switches a plugin in | implementer's call |
| **BE** | A cue with no insert switched in still shows the page, saying "no FX in", so the press is never silently ignored | implementer's call |
| **BF** | The page is built on one plugin's parameter list - `/godot/plugin/<pid>/param/<n>/…` for the names, the steps and the default, and a writable 0..1 node for the value - so the live rack's plugins (Phase 9b) take the same page | implementer's call |

**The rows.**

| Node | Type, default | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/surface/<id>/page` | `s` | ro | none | gains `fx` (§16.2); `pageIndex` and `pageCount` count across every insert's pages, `edited` is the `p<n>` last turned |
| `/godot/fx/<id>/live` | `s` | ro | none | the parameters a locked show is riding live on this insert, by index, space-separated (BB) |
| `/godot/fx/<id>/values` | `s` | rw | show | publishes the values **heard**: under the lock, what rides live over what the show says, so the plugin's own window and the panel follow a turn |
| `/godot/fx/<id>/p<n>`, `t<n>` | | | | the value heard and the plugin's text for it, as the row |

**The door.** `cue::fxWriteFor` takes the live layer: under the lock a write to `p<n>` is refused
as it would be unlocked (a value outside nought and one, a word, a parameter the catalogue says the
plugin does not have, an insert nobody made) and otherwise held in `cue::LiveEdits`, per insert and
per parameter; the show's own value back again drops what rode there. Once unlocked, a write is the
show's and drops what rode at that parameter only. `cue::isLiveEdit` answers yes to a `p<n>` write
under the lock, so it opens no transaction. `live.keep` merges each insert's riding values into its
`values` row in the same one transaction as the EQ and the sends, and skips an insert deleted
since; `live.drop` lets them go, and a parameter the show never set goes back to the preset (-1 to
the voice). `Runner::fxOf` lays the layer over the row, so an arm carries what is heard, and
`applyFx` is gated on the layer's revision as well. `/godot/document/live` counts a parameter as
one change.

**The bridge.** The FX button (note `0x2B`, the Mackie "Plug-In") is `Action::fxPage`. A page is
one plugin's parameters from `first`, as many as the surface has rotaries; the display shows the
parameter's name (the short name on a Mackie's seven characters) and `t<n>`; the ring fills from
the centre for a parameter the catalogue guesses bipolar and from the left otherwise, in the
insert's colour from the EQ band palette. A turn writes `node.set /godot/fx/<id>/p<n>` with origin
`surface:<id>`, so undo's coalescing makes a turn one step per parameter; a continuous parameter
moves `pageParameterTravelPerDetent` (1/128) of its travel a detent, a stepped one a step. The
window's foot follows the page onto the cue's FX panel.

**What it costs, said as it was chosen.** A press puts the plugin's own default, not the preset's
value: a set entry that loads a preset whose value differs is not restored by the press, and
withdrawing a value from the row (so it rests at the preset) stays a job for the window. The
tree's `p<n>` for a parameter the cue does not set shows the plugin's default while the voice
plays the preset's, as it did before the page (§17.4). Two sixteen-parameter pages of an insert
with hundreds of parameters are a long walk; that is what the curated maps are for.

**Built** (2026-09-26, on `main`): the page, the bridge's FX paging, the rings and the parameter
law (`surface/SurfacePages`), the page word in the schema and the client's foot (4dd9b28); the live
layer's plugin values, the door, the Runner and tree overlays, `fx,live`, and Keep and Discard
carrying them (9e4bfdc). **Owed to the bench:** whether a hundred-and-twenty-eighth a detent
feels right on a real reverb, how the plugin names read cut to the D700's field, the FX LED, and
the plugin's own window following a turn under the lock.

### 17.17 The D700's arrows move the standby, and a park on a cue that is not a stop lands on its group (2026-09-26)

Asked by the author on 2026-09-26: *"Can the up(-left) and down(-right) arrows on the D700 be used to
move the standby cursor (playhead) to the next playable cue? Can pointing it at non triggerable cue
(like a sample cue in a sampler group) move the pointer to the group instead of showing an error?"*
Both answered yes and built the same day. The letters go on from §17.16's BF.

| | Decision | Whose |
|---|---|---|
| **BG** | **The D700's ↖ ↘ arrows are `standby.previous` and `standby.next`**, as ◀◀ ▶▶ are on a Mackie. They send the Mackie bank notes (`0x2E`/`0x2F`), and the D700 has no rewind or forward, so until now nothing on it moved the pointer but GO | the author's |
| **BH** | **`standby.set` on a sampler group's member parks on the group** instead of answering `not-a-stop` - PRD §3.27's flagged sentence, answered | the author's |
| **BI** | The same for a **header's or a footer's cue**: it parks on the group the section belongs to, and the desktop's gutter sends the group for those rows (a band's too) instead of saying a sentence | implementer's call, from the author's "non triggerable cue (like …)" |
| **BJ** | On a **Mackie** the bank arrows still do nothing: it has the transport's pair, and banking is §3.9d's decision with the hardware in hand. On the D700 the arrows move the standby on **every** page - the FX page turns with FX pressed again (§17.16), so they are free there | implementer's call |
| **BK** | What is **still refused** `not-a-stop`: a cue of a **persistent** section (its parent is the list; there is no group to stand for it) and a **disabled** cue, which is refused for what it is rather than where it is and is not lifted onto its group | implementer's call |
| **BL** | **The document's own door stays strict.** A `node.set` of `/godot/list/<id>/standby`, and a state file restoring one, naming a member is still refused: a value written to the node is a value, and a door that quietly stored a different one would be one nobody could reason about. Only the command - a gesture - lands on the group | implementer's call |

**Where it lives.** `cue::nearestStop (list, cueId)` in `cue/CueList`: the cue itself when
`mayStandOn` says yes; otherwise, for an enabled cue of this list, the first enclosing cue element
the pointer may stand on; otherwise empty. `standby.set` asks it once `mayStandOn` has said no and
the cue is in this list; a cue of another list is still `not-in-list`. The command's reply echoes
the argument it was sent, as before - the published standby says where the pointer went. The walk
is unchanged: `standby.next` never entered a bank or a section, and still does not.

**On the surface.** `surface::actionFor` maps the bank notes to `Action::rewind`/`forward` for the
`d700` profile only, so the bridge sends the same two commands ◀◀ ▶▶ already sent. The ↖ arrow is
back (previous), ↘ forward (next). Neither lights: the D700 has no local LED feedback and nothing
asks for one.

**Owed to the bench:** the arrows pressed on the D700 itself - the notes are read from the protocol
note (`godot-asparion-d700-protocol-0.1.md` §2.1), not yet from a press.

### 17.18 The master dial: the number last clicked in the window (2026-09-26)

Asked by the author on 2026-09-26: *"Can selecting a parameter in the inspector or foot panel
on-screen via mouse or touch assign it to the master rotary encoder on the D700?"* Four questions
(AskUserQuestion), each answered as recommended, and one answer revised while it was being built:
*"The D700 can register a double click, in this case click could be deselect and double click back
to default"* - *"The D700 can do it at hardware level."* The letters go on from §17.17's BL. The
pages draft's §5.4 proposed the dial walk the cue list; this replaces that.

| | Decision | Whose |
|---|---|---|
| **BM** | **Any click or touch on a number** in the inspector or the foot panel puts it on the master dial - no gesture of its own | the author's |
| **BN** | **It stays on that cue's row** when the pick moves; only another click, or the dial's own click, moves it | the author's |
| **BO** | **The dial's click lets go; its double click puts the number back to its default** - the D700's firmware telling the two apart (revised from the first answer, "press = back to its default") | the author's |
| **BP** | **Free, it does nothing and is dark** | the author's |
| **BQ** | What a click can put on it: a number the show stores and a hand may write - one value, `d` or `i`, not a closed set, not a switch, not a word. Its law is read off its row (below), so a row added to the table turns with no line written | implementer's call |
| **BR** | The double click is **F4, note `0x39`**, by the `*` button's pattern (F1 single, F2 double); the dial's click is F3, `0x38`. It needs **double click ticked for the dial in the Configurator**, and the note is to confirm at the bench | implementer's call |
| **BS** | A **Mackie's jog wheel** turns the same number; its F3 and F4 stay its own function keys | implementer's call |
| **BT** | **Under the lock the row's own door decides**: an EQ band or a send rides live (AM), anything else is refused `locked` as it would be from the window | implementer's call |
| **BU** | **What says where it is**: the dial wears its cue's colour (white for a number with no cue of its own, dark while free); in the window the line wears **◉ before its name and a frame round its value**, a send strip ◉ before its name, and the transport's foot row says **"◉ Kick: level -6 dB"** wherever the number is - a mark and words, never the colour alone (§4.8) | implementer's call |
| **BV** | **Where a click is heard**: the inspector's lines (name or value), the EQ panel's boxes and the send mixer's strips. Not the EQ picture's handles (a handle is two numbers), not a plugin's own window (another process). A field over several picked cues gives the dial its first cue's row. A click is sent only when the show has a Mackie or a D700 switched on and the number is not on the dial already, so nothing is logged for nothing | implementer's call |

**The rows and the command.**

| Node | Type, default | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/surface/dial` | `s` | r | none | the address the dial turns, or empty; empty too once what it named is gone (an Undo of the delete brings it back) |
| `/godot/cmd/surface/dial` | `s` | w | — | `surface.dial <address>`; empty frees it. Refused `bad-address` for an address that names nothing and `bad-value` for anything BQ leaves out. No step of the history, allowed under the lock |

**The law, `surface::dialTurned`.** A frequency whose floor is above nought turns in ratios, a
sixteenth of an octave a detent, as a band's rotary does; a decibel that reaches silence
(`-120`) is a level and moves along the fader, a narrower one a gain in half decibels; a time in
seconds a tenth a detent, and a whole second from ten seconds up; a number with no unit that
spans a hundredfold (a Q) in ratios; any other number with two ends a hundred-and-twenty-eighth
of its travel; a whole number one a detent. All held at the row's ends. The command reads the row
once and keeps what the bridge needs in the surface table, so the tick never asks the document.

**The bridge.** The jog's detents (CC `0x3C`, sign and magnitude) are folded into one `node.set` a
tick, with the surface's origin, so a turn is one undo step per hand. The double click writes the
row's default, and writes nothing when the number is already there. The dial's colour is sent on
the first port, at a strip's pace (`colourIntervalTicks`, re-asserted every
`idleColourReassertTicks`).

**Built** (2026-09-26, on `main`): the command, the row, the law, the bridge and the light
(85f16db); the window's clicks, marks and the transport's line. **Owed to the bench:** the jog's
CC and the sign of its turn, the double click's note with the Configurator ticked, whether a tenth
of a second a detent is the right grain for a pre-wait, and the dial's colour.

## 18. Phase 9b — the live rack: mic cues, rack channels and named inputs: what the tree, the commands and the log gain

Written on 2026-09-26, before the code, as §11 to §17 were: the approved plan drawn as a text the
stages 9b.1–9b.7 can be reviewed against rather than against memory. It is drawn against `main` at
`b284a4e`. Rows reach `docs/parameters/godot-parameters.csv` with the stage that implements each of
them, never before. Where this section and the code come to disagree, §18.12 at close-out says
which won. §19, written the same day, draws the live sampling channels built on top of it.

The request was the author's, on 2026-09-26: *"At this point could we add the effects rack for live
inputs? And with a follow up, not yet in the PRD for live sampling channels that can take in an
input, loop with continuously variable in and out points with pre-recording and
post-looping/playback effects."* The first half is this section; the second is §19.

**What the phase is, before any of its names.** Until now every sound Go.dot made came from a file.
A live input — a microphone, a keyboard's line, the output of another machine — becomes a cue like
any other: the **mic cue**, QLab's word for it, whatever is plugged in. The show **names its
inputs** as it names its outputs, each with a meter, so a dead microphone is seen before GO. It
declares **rack channels** by name — Vox 1, Vox 2, Band — each a track of its own beside the voices,
with a width and **its own chain of plugins**, loaded switched off when the show opens. A mic cue
names an input and a channel; GO opens that input through that channel with the cue's level, its
EQ, the plugins it switches in, its outputs and its sends — everything a media cue has, with an
input where the file was. It runs until something stops it; one that must run all show sits in the
persistent section (PRD §3.29: *"a sampler bank, a rack chain and a state machine differ only in
what starts them"*). The plugins run out of process as the voices' do. Nothing is compensated: the
delay from the microphone to the output is said in words, against a budget the show sets.

Five decisions the author took and nine calls of the implementer's shape it — §18.1; the six
sampling decisions of the same day are §19.1's.

**Where it starts, in the code rather than in the plan.** The inputs already arrive. The show picks
an input interface (`audio/@inputDevice`) and a patch from logical inputs to hardware
(`audio/@inputPatch`, `src/wfg/engine/audio/AudioSettings.cpp:59-72`); `DeviceLayer` opens them
(`DeviceLayer.cpp:339-467`) and hands every block to `AudioHost::processBlock` patched
(`:201-203`), which copies them into the buffer it gives Tracktion (`AudioHost.cpp:712-715`) and
tells Tracktion's hosted interface how many there are (`:226`). And there they stop: Tracktion is
told of no input device (`describeWaveDevices`, `:89-92`), so it builds none and the samples are
overwritten by the outputs. Tracktion's own input path is not the way in: its `WaveInputDevice`
takes two `juce::CriticalSection` locks a block and sizes a thirty-second retrospective buffer on
the audio thread (`tracktion_WaveInputDevice.cpp:137-222, 1226, 1432-1433, 1784`), which PRD §4.2
forbids and the rtsan job would refuse. Every voice track is `EqPlugin` → a `ProxyPlugin` per
entry of the set → `CueOutputPlugin` (`AudioHost.cpp:394, 415-433, 435-451`), delay compensation
off (`:320`), and `trackCount()` — the size of that list (`:1666-1669`) — is the Runner's
polyphony (`Runner.cpp:2568, 3742`). Phase 4 left `Audio/Rack/Channel` (a name, a class, an access;
`ShowDocument.cpp:1392-1429`) and a media cue's `Insert` as bookkeeping: `claimSlotsFor`
(`Runner.cpp:2417-2494`) claims the channel and nothing sounds through it. Tracktion builds no node
for a track with no clip and no input unless one of its plugins produces sound without input
(`tracktion_EditNodeBuilder.cpp:1600-1614`); `CueOutputPlugin` already says it does
(`CueOutputPlugin.h:117`), so a track with none of either is fed a two-channel silence.

Three rules hold over all of it. **GO never blocks** — a mic cue waiting for its channel shows
*pending* in words and GO has already returned; no plugin is made after the show opens; a channel or
a plugin added mid-session waits for Load now. **The audio thread is a lipogram** — the inputs are
copied into a buffer Go.dot set aside at start, inside Go.dot's own measured region and before
Tracktion is called, and the stage that reads them allocates nothing and locks nothing. **The
document holds what somebody decided** (§4.10) — an input's meter, a channel's holder and a path's
delay are readings, persisted nowhere; the input a cue takes and the channel it goes through are
decisions, saved.

### 18.1 The decisions (2026-09-26)

Asked directly (AskUserQuestion), each with a recommendation beside it, all taken. The letters go on
from §17.18's BV. The sampling decisions asked the same day — BZ, CA, CB, CC, CD, CF — are §19's and
listed there.

| | Decision | Whose |
|---|---|---|
| **BW** | **A live input is a cue** — it names an input; GO opens it with its level, fade-in, outputs, sends, EQ and inserts; it runs until a stop cue, Esc, a fade that stops or its group's footer ends it; one that must run all show sits in the persistent section | the author's |
| **BX** | **Named rack channels, each with its own chain** — declared by name with a width class and its own plugins, all carried switched off; a mic cue names its channel and switches in what it wants; **a cue wanting a channel another holds waits and says so** | the author's |
| **BY** | **The latency budget is five milliseconds of plugins**, a setting of the show; the whole delay from the microphone to the output is always said in words; going over is said loudly — on the channel, the cue and the plugin that did it — and never refused, never compensated | the author's |
| **CE** | **The kind is called Mic**, QLab's word for any live-input cue. *Live* is the lock's own row on every cue (`/godot/cue/<id>/live`, §17.14) and *Input* names the inputs | the author's |
| **CG** | **A stopped mic cue's tail rings out**: the input shut, the plugins ringing until the channel is quiet — ten seconds at most — and then the channel is free. A cue waiting for it waits for the tail too; double Esc cuts everything | the author's |
| **CH** | **Go.dot's own input stage**, never Tracktion's input devices: the block's inputs copied into a tap before Tracktion runs, read by `LiveInputPlugin` at the head of each rack track in the same block. No delay added, the input a number, never a rebuild | implementer's call |
| **CI** | **Named inputs are show objects mirroring buses**: `Audio/Input`, a name and a width, packed onto the logical inputs by the buses' arithmetic; the input patch follows the list until the rig settles it | implementer's call |
| **CJ** | **`Mic` is an element of its own** carrying owners `{ cue, sound, mic }`: the thirty-five rows a media cue and a mic cue share move from owner `media` to a new owner `sound`, addresses and show files unchanged | implementer's call |
| **CK** | **Rack channels are tracks after the voices**: `trackCount()` stays the voices; the lists of matrices, EQs and output stages span both; a rack track is built two channels wide | implementer's call |
| **CL** | **One child process per distinct plugin and preset across the rack**, a lane per channel that uses it — a child spins a core while any of its lanes is switched in, so a child per channel slot would spend a core per slot | implementer's call |
| **CM** | **A waiting mic run holds no track** until its channel is granted, then a hook arms it; a shared channel, an input too wide for the channel's class and a missing input are refused in words | implementer's call |
| **CN** | **Double Esc silences a mic cue**, never leaves it dry — §4.4's *"kills all internal processing including live effects"* | implementer's call |
| **CO** | **A channel's delay is what is switched in.** The proxy keeps no delay line for a plugin that is out, unlike the Tracktion bypass PRD §3.18 describes; over the budget is a reading, published and never logged | implementer's call |
| **CP** | **A media cue's `Insert` stays bookkeeping** and no longer claims a channel that now sounds; `wfg validate` says it does nothing | implementer's call |

**BW — a cue, not a panel.** The alternative offered was LiveProfessor's own shape: a list of
standing channels in the show settings, each passing sound whenever the show is open, cues only
adjusting them. It was declined because of §3.29's sentence — everything that runs has a row, a run,
a lifetime a footer ends, the three stops and the solver. A standing channel is what the persistent
section already gives a mic cue for nothing, so both behaviours are one mechanism.

**BX — named channels, not voices and not a pool.** Two alternatives were offered. On the playback
voices, with the show's plugin set: nothing new to declare, but every plugin the rack needs carried
on every voice (the N × P of §17.9's M34) and a mic cue waiting behind playback for a voice. A pool
of identical rack channels carrying one rack set: cheaper, but no channel with a name or a purpose.
The author chose PRD §3.18 as it was written on 2026-09-07: a channel is somebody's decision about
the rig — *Vox 1 is a compressor and a reverb* — and a live input never competes with playback.
The cost is a claim that can find its channel held, and that is answered by waiting: a mic cue that
wants to change a sounding microphone's processing without a gap uses a second channel on the same
input and crossfades, since an input is not a slot and two channels may read one.

**BY — five milliseconds.** What the plugins add, on top of the interface's own delay: room for a
look-ahead compressor or limiter on a voice, while a linear-phase EQ or a pitch corrector goes over
and says so. Three and ten were offered.

**CG — tails.** *Cut at the stop* was offered and declined: a reverb or a delay cut off at the stop
is the sound of a mistake. The price is a channel held a little longer after a stop.

### 18.2 The objects and their rows

**A named input is `<Input>` in an `<Inputs>` container under `<Audio>`**, owner `input`,
published at `/godot/input/<id>` with `/godot/input/order` beside it — the `Plugins` shape for the
container, made on demand at a fixed place after the buses, so an input's position counts from
nought whatever the buses are doing and the canonical bytes do not depend on which container was
asked for first. The input itself is the `Bus` shape exactly, for PRD §3.9b's reason — *"Voix solo"*
is what somebody wrote down and *input 3* is a fact about a patch that will change — so a name, a
width, and a first channel that nobody types: the running sum of the widths before it, kept so by
`input.create|delete|move|width` through `document/OutputLayout.h`'s arithmetic, which never knew it
was drawing outputs. The first channel is a **logical** input; `audio/@inputPatch` maps
logical to hardware as it always did, and follows the list while the show is fresh exactly as the
output patch follows the buses (`audio/@inputPatchSettled`, the twin of `patchSettled`). Inputs are
not slots: two channels may read one, which is how a mic changes processing without a gap.

**A rack channel is `<Channel>` under `<Rack>`, as Phase 4 left it, with `<Plugin>` children** — the
same element, and so the same `plugin` rows, as an entry of the show's set: a name, the scan's
identifier, a format, a path and a preset. The children are the channel's chain in order. A channel
is published where Phase 4 put it, `/godot/slot/<id>`, and its plugins at `/godot/plugin/<id>` beside
the set's, with their states, their problems, their declared delays and their catalogues;
`/godot/plugin/order` stays the set's, so the voices' panels never show a rack plugin.

**A mic cue is `<Mic>`**, `{ cue, sound, mic }`, with the children a media cue's sound is made of —
`Route`, `Send`, `Feed`, `Fx`, `Trigger` — and without the ones that name a file: no `Range`, no
`Insert`. Its `Fx` entries name plugins of **its channel**, never of the set, and carry what a media
cue's carry: whether each is switched in, its values, its whole state (§17.13).

**The owner split.** Thirty-five rows move from `media` to `sound`, the owner of what a cue that
sounds carries whatever its source: `level`, `stereoToMono`, `directOut`, `sharedOut`, `dca`, the
twenty-three `eq…` rows, `fx`, `live`, `sends`, `outsBusy`, `outsMaybe`, `chainChannels`,
`insertLatency`. `media` keeps `file`, `startOffset`, `channels`, `duration`, `hash`,
`initialLevel` and the nine sampler rows. The addresses do not move — every row is still
`/godot/cue/<id>/<row>` — and a show file does not change by a byte, the canonical writer sorting
attributes by name (`document/CanonicalXml.cpp:158-190`). The trap is the reader:
`Reader::text` falls back to a default keyed `owner/row` (`cue/ShowWalk.h:111-129`), so a read left
asking `media` for a row that moved returns an empty string rather than an error. The split is
therefore its own commit, proven by byte-identical tree dumps and replays of every fixture before
`Mic` exists.

*As built (9b.4, 6dcf8a5).* As drawn. `wfg tree` of all 28 fixture bundles is byte-identical before
and after, every fixture log replays in both locales, and `show.rng` did not change. The reads that
had to follow: the Reader's owner list, the Runner's EQ reads, the fold and the direct out, the slot
analysis, the solver's level, `eq.reset`'s walk of the EQ rows (which would have become a silent
no-op), the tree's rows for a media cue, and two test helpers. A `RunTests` case is the tripwire: a
cue nobody shaped is armed, and every EQ number and the level are compared with the table's
defaults - put one Runner read back to `media` and it fails.

*As built (9b.4), the element.* `Mic` as drawn, published with the `cue`, `sound` and `mic` rows
and nothing about a file. Its inserts are looked up in its channel (`insertChainOf` in the tree),
and `fx.create` now checks the list for both kinds - which closed a hole 9b.3 had opened: a
channel's plugin is a `Plugin` like any, and a media cue could switch one in. A route, a feed, a
send and an insert take a mic cue; a range, a split and an `Insert` do not; deleting a bus lets go
of a mic cue's direct out as of a media cue's. Until 9b.5 a mic cue fired is a line with a run, as
a memo is, so nothing waits on a run nothing would end. The inspector offers the input and the
channel as menus (`inputRef`, `channelRef`), each item saying the width or the class the other must
fit; the fold stays greyed on a mic cue until its chain's width is published (9b.5).

**The containment**, in `Schema.cpp`'s table:

```
{ "Audio",   false, { "Bus", "Inputs", "Rack", "Plugins" },           { "audio" } },
{ "Inputs",  false, { "Input" },                                       { "inputs" } },
{ "Input",   true,  {},                                                { "input" } },
{ "Channel", true,  { "Plugin" },                                      { "slot", "rackChannel" } },
{ "Media",   true,  { "Route", "Send", "Feed", "Insert", "Range", "Trigger", "Fx" }, { "cue", "sound", "media" } },
{ "Mic",     true,  { "Route", "Send", "Feed", "Fx", "Trigger" },      { "cue", "sound", "mic" } },
```

and `Mic` joins the children of `List`, `Group`, `Header`, `Footer` and `Persistent`.

The rows, by where they are published. Every one carries the panic policy `park`, and rests at its
default.

| Node | Type, default | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/input/order` | `s` | r | none | the named inputs by first logical input, then identifier — what every input menu offers |
| `/godot/input/<id>/name` | `s` | rw | show | what the input is called — *Voix solo* — in every menu |
| `…/width` | `i`, 1 (1..8) | r | show | how many consecutive logical inputs it takes; changed by `input.width` |
| `…/firstChannel` | `i`, 0 | r | show | its first logical input, the running sum of the widths before it |
| `…/meter` | `d`, −120 dB | r | none | the loudest sample on any of its channels over the last tick, whether or not a cue listens — the soundcheck's question, *is the mic alive?* |
| `…/problem` | `s` | r | none | why it is not reaching Go.dot, in one sentence: *input 9 is not on this interface* |
| `/godot/audio/inputLatency`, `outputLatency` | `i`, 0 samples | r | none | the interface's own delays as its driver reports them, read when the device opens |
| `/godot/audio/rackBudget` | `d`, 5 ms (0..100) | rw | show | what a mic cue's plugins may add before they say so (BY) |
| `/godot/audio/inputPatchSettled` | `T`, false | rw | state | the input patch no longer follows the list of inputs (the `patchSettled` rule) |
| `/godot/slot/<id>/plugins` | `s` | r | none | a rack channel's plugins by id, space-separated, in the order of its chain. Each is published at `/godot/plugin/<id>` with the set's rows — name, state, problem, latency, layout, preset — and never in `plugin/order`, which stays the chain on every voice |
| `…/latencySamples` | `i`, 0 samples | r | none | what its chain declares with every plugin in — the worst case, shown when somebody adds a plugin rather than discovered on the night. In samples, as a plugin's own row is; the Rack tab says it in milliseconds against the budget |
| `/godot/cue/<id>/input` | `s`, `refers=input` | rw | show | on a mic cue: the input it takes |
| `…/channel` | `s`, `refers=rackChannel` | rw | show | on a mic cue: the rack channel it goes through |
| `…/fadeIn` | `d`, 0 s (0..) | rw | show | on a mic cue: how long GO takes to bring it from silence to its level; nought opens it at once, behind a click-free ramp |
| `…/latency` | `d`, 0 ms | r | none | on a mic cue: the whole path — the interface's input and output delays and what the plugins it switches in declare |
| `…/overBudget` | `T`, false | r | none | on a mic cue: its plugins add more than `rackBudget` |

`cue/kind` and `run/kind` gain `mic`. `run/state` gains nothing: a mic run waiting for its channel
is `armed` with no track and the channel in `run/pending`, as a sampler member waiting for a voice
already is with `voice` there (`Runner.cpp:2576-2582`); one ringing out after its stop is
`stopping`.

### 18.3 The commands

| Command | Arguments | What it does, and what it records | What it refuses |
|---|---|---|---|
| `input.create` | `<width i> <index i> [id s]` | a named input at a place in the list, repacked; the patch follows while the show is fresh | a width outside 1..8; `locked` |
| `input.delete`, `input.move`, `input.width` | `<input s>`, `<input s> <index i>`, `<input s> <width i>` | as the buses' three | an input the show does not have; `locked` |
| `channel.plugin` | `<channel s> <name s> <identifier s> <format s> <path s> [id s]` | a `<Plugin>` at the end of a rack channel's chain. `plugin.create` could not grow the argument: its identifier is its trailing optional one, and a log written before would read the channel as the identifier | a channel the show does not have; `locked` |
| `cue.create` | as today | gains the kind `mic` | as today |
| `fx.create` | `<cue s> <plugin s> [id s]` | on a mic cue the plugin must be one of its channel's; on a media cue, of the set, as before | a plugin of the wrong list — `bad-value`; as today otherwise |
| `eq.reset` | `<cue s>` | accepts a mic cue | as today |
| `plugin.load` | — | Load now rebuilds the rack as it stands with the set | while anything plays — a sounding mic counts, and the refusal names it |

Moving a channel's plugin is `object.move`; removing one, `object.delete`.

`wfg validate` (as built, 9b.4) says, as warnings, each reason a mic cue would fail when fired: it
takes no input; it plays through no rack channel; its channel is shared (`bad-channel`); its input
is not as wide as its channel takes - mono and mono-to-stereo take one channel, stereo two
(`bad-width`). An input or a channel naming the wrong kind of thing is the `refers` check's.

### 18.4 The input, end to end

1. **The interface.** `DeviceLayer` patches the hardware inputs into logical ones as today.
2. **The tap.** `AudioHost::processBlock` copies the block's logical inputs into a buffer set aside
   at `start` — every input it was told of, zeros where a block brings none (the hosted pump, the
   arm's own readiness wait) — and keeps each channel's peak for the tick. Inside Go.dot's region
   of the lipogram check, before Tracktion's call; the buffer Tracktion is handed cannot be read
   instead, because the hosted device uses one buffer for its inputs and its outputs and clears
   the outputs before it renders (`tracktion_HostedAudioDevice.cpp:83-86`,
   `tracktion_DeviceManager.cpp:1399-1401`).
3. **The input stage.** At the head of every rack track, `LiveInputPlugin` copies from the tap the
   channels its cue says — a first logical input and a width, two atomics the tick thread writes —
   behind a **gate** that opens and shuts on a click-free ramp at a sample Go.dot places. It reads
   the block's own start and length, never an assumed whole block, and ramps in after a gap in the
   blocks, so a device that went away and came back (§6.2) does not open with a step.
4. **The chain.** `EqPlugin`, then a `ProxyPlugin` for each of the channel's plugins in order, then
   `CueOutputPlugin` — the voice's own three, unchanged: level, matrix and peak.
5. **The outputs.** The matrix sends the channel where the cue's direct out, routes, sends and
   feeds say, the coefficient trick of every voice.

The input of block *n* is heard in block *n*: nothing here adds a block. What remains is the
interface's own delay and what the plugins declare (§18.7).

**Widths.** A rack track is two channels wide whatever its class. **Mono** takes one input and puts
out one; **mono → stereo** takes one and may put out two, when a plugin in its chain widens it as a
voice's insert does (§17.15, decision AT); **stereo** takes two and puts out two. A mic cue's input
must fit: a mono input goes through a mono or a mono → stereo channel, a stereo input through a
stereo one. The routing reads the width after the chain, as for a media cue.

### 18.5 A mic cue's life

**Arm.** The cue claims its channel through `claimSlotsFor`, above the no-audio return, as every
claim is made, so a replay makes the same one. A channel held by another run: the claim waits in
the queue, and the run **holds no track** (CM) — a run with a track would receive every live push
the Runner makes, and would move the sounding cue's matrix, EQ and plugin values. It is `armed`
with no track, the channel in `run/pending`, and the running pane says *waiting for Vox 1*. When the holder's run
ends, `releaseSlotsOf` grants the claim in the order runs were made, and a hook submits `run.arm`,
the sampler's voice wait exactly (`Runner.cpp:3738-3761`). A channel the graph was built without —
declared since the show opened — fails the run `not-built` with *Load now to build Vox 3*. A shared
channel, an input the channel's class cannot take and an input the show does not have fail it
`bad-channel`, `bad-width` and `no-input`, each with its sentence; `wfg validate` says all four
before the show. Holding its channel, the run takes that channel's track and the arm snaps the
routing, the EQ and the channel's plugins as the cue sets them — values, switches and whole state
(§17.13), the launch waiting until they have settled — and resets the plugins, which on a quiet
channel cuts nothing. The gate stays shut.

**Launch.** The gate opens at the launch's sample. The level goes from silence to the cue's over
`fadeIn`, a fade like any other; nought opens it at once behind the gate's ramp.

**While it sounds.** A mic run is a run holding a track: the Runner's live pushes of level, routing,
EQ and plugin values reach it as they reach a media run, the lock's live layer with them (§17.14,
§17.16), the rotaries' pages with it.

**Stop.** A stop cue, Esc, a group's footer or the end of a fade that stops: the input is shut — over
the stop's fade when the verb is `fade`, the fade moving the level *into* the chain rather than out
of it — and the channel's plugins ring out at the cue's level until the channel has been quiet for
a quarter of a second, or ten seconds have passed (CG). Then the run ends and the channel is free.
A fade cue that does not stop moves the cue's own level, the output, as for a media cue.

**Kill.** Double Esc, or a kill from the running pane: the input shut, the level to silence at once
behind the matrix's own ramp, the channel's plugins reset so that the next cue on the channel does
not open onto a tail left inside them, and the channel free at once (CN).

**Persistent.** A mic cue in the persistent section is asserted as a media cue is; its resume is a
relaunch, having no position to remember. §18.8 is what that needed.

**Load now and a change of interface** are refused while a mic cue sounds, as while anything plays,
and the refusal names the cue — a persistent mic would otherwise refuse them all show with no clue
why. A double Esc clears the way and the next GO restores the section.

*As built (9b.5).* The life above, with these particulars. **The claim** is made in `claimSlotsFor`
with a processor input's policy - queued behind the holder, never played dry - and a waiting run
holds no track; `armWaitingMics`, on the tick with an audio side, sends `run.arm` once when the
claim lands, and `armAgain` takes a mic cue through `armMic`. **The failures** - `no-input`,
`bad-channel` (no channel, or a shared one), `bad-width`, `not-built` - are `run.failed` records
from below the audio return, so a replay reads them from the log. **The arm** carries `live`, the
input's first logical channel and its width, no file and no ranges; `fxOf` reads the channel's chain
as the graph built it, `chainOfCue` has a mic branch (the input's width at the head of a two-channel
track), `resolveRouting` is given the rack track's two channels, and `sourceChannelsOf` answers the
direct out's and the sends' spread with the input's width where a media cue's gives its file's.
**The launch** is `Player::openLive`: the gate opened at the launch's own sample over the cue's
`fadeIn`, *on the gate's own ramp* - equal power, the gain the sine of how far along it is - and
**not** a fade job, which is where this departs from the text above: nothing else can take the
fade-in over, and the level a fade would move is the cue's level all along. **A stop**
(`HostPlayer::stop`) shuts the input in five milliseconds; the channel reads as playing while what
reaches its output stage - the chain's output, before the level - has crossed -60 dBFS inside the
last quarter of a second, or until ten seconds since the shut (`AudioHost::isRackSounding`, fed by
the output stage's quiet count); then `observeEdges` ends the run and the claim goes to the next in
the queue. **A fade that stops a mic cue** closes the input over the fade (`Player::shutLive`) and
its job keeps the level where it was, on the session and on a replay alike. **A kill** - any run
whose `skipFooter` is set, which is double Esc and the running pane's kill - is `Player::kill`,
which on a voice is the stop it always was and on a rack channel is `killRack`: the input shut in a
millisecond, the level to silence, every lane reset, nothing left ringing. The routing, EQ and FX
pushes reach a sounding mic run as a media run's. **Not in 9b.5:** a mic cue armed at standby or
prepared by a header, asserted in the persistent section, or named by the refusal of Load now (all
9b.6); and the path's delay and `overBudget` rows, which §18.7 makes readings the tree derives and
the window says - with the window, in 9b.7.

### 18.6 The rack's plugins and their children

Hosted as the set's are (§17.6): out of process, behind the proxy, with the same region, lanes,
deadline and failure rules. **One child per distinct plugin and preset** across every channel of
the rack, a lane for each channel that has it (CL) — the child's worker spins a core at the audio
thread's priority while any lane is switched in (`PluginHostChild.cpp:176-182`), so the number of
spinning children is the number of distinct plugins in use, not the number of channel slots. A
lane count is fixed when a child starts (`SharedRegion.h:208-251`), which is one more reason a plugin
added to a channel waits for Load now.

**A failure** fails the child, and so the plugin on every channel that has it: each passes it dry,
and the words name them — *Comp stopped answering; Vox 1 and Vox 2 play without it*. One automatic
relaunch, then `plugin.restart`, as for the set.

**The window.** *Edit…* opens the plugin's own window in the editing helper, one per plugin, which
follows the pick onto any mic cue whose channel has that plugin and greys otherwise (§17.13). The
FX page (§17.16, decision BF) was built on one plugin's parameter list for this: a mic cue's inserts
take the rotaries as a media cue's do.

**The cost, said as it was chosen.** A mic cue in the persistent section with a plugin switched in
keeps that plugin's child spinning all show. M40 counts the cores.

### 18.7 Latency, and the budget

A mic cue's **path** is the interface's input delay, its output delay — both as the driver reports
them when the device opens, carried on the logged `audio.settingsReady` so that a replay reads what
the session read — and what the plugins it switches in declare. **Not the ones switched out**: the
proxy does not call a lane that is out and keeps no delay line for it, unlike the Tracktion bypass
PRD §3.18 describes (CO), so switching a latent plugin in or out moves the channel's timing, and the
words say so when it happens. Nothing is compensated, the plugin's delay arriving inside the
proxy's answer as it does on the voices.

**The budget** is `audio/rackBudget`, five milliseconds of plugins (BY). Over it, the cue's row in
the list and the inspector, the channel in the Rack tab, and the plugin's box at the foot each say
so — in words and a mark, never colour alone (§4.8): *Vox 1 is 7.3 ms from the microphone to the
output; its plugins add 5.8 ms, over the 5 ms budget: Pro-L 2 adds 5.0 ms*. The channel's own
`latencySamples` row is the worst case, every plugin in, shown the moment somebody adds a plugin
rather than discovered on the night. Over the budget is a reading the tree derives from the plugin table
and the device, like a plugin's state; it refuses nothing and is logged nowhere.

### 18.8 Two faults in the persistent section, one in the jump, and one in the player

A mic cue that must run all show is the persistent section's first real tenant, and two things
there did not do what §3.29 says.

- **A double Esc suspended every persistent cue for the session.** `run.killAll` marks every root
  run `killed` (`RunCommands.cpp:646-647`), and `assertPersistent` reads a killed run as the
  operator's kill from the running pane, which suspends its cue until a load-to-time lifts it
  (`Runner.cpp:1111-1127`). PRD §3.29 says a double Esc does not suspend, *"the next GO restoring
  the declared world is the point of declaring it"*. Now `run.killAll` marks runs as skipping their
  footers and not as killed; the running pane's kill still suspends.
- **A jump cut the section's runs.** Load-to-time's sweep ends every run of the list the plan does
  not name (`Runner.cpp:1241-1263`), and the solver never plans the persistent section, so a jump
  ended a sounding bed — and a jump is not a step, so nothing asserted it again until the next GO.
  Now the sweep leaves the section's runs alone.
- **A jump doubled a cue it planned that was already running** — found by recording the fixture
  below, once the first fix held. The sweep passed over every run of a cue the plan names, and
  `seatPlan` builds every planned cue afresh (a jump hands it an empty map), so the old run stood
  beside the new one: a playing cue sounding on under its own relaunch, where PRD §3.25 says a cue
  at the wrong offset is stopped and relaunched, or the old standby's armed run holding a voice.
  With two tracks and a bed now keeping one, the jump's own cue failed `no-track`. Now the sweep
  ends every run of the list outside its persistent section, the plan's own cues included.
- **`HostPlayer::isPlaying` asked slot 0 alone** (`HostPlayer.cpp:241-244`), where the interface
  promises any slot and `AudioHost::isTrackPlaying` checks every one (`:1378`). The Runner forgives
  the silence of a range that is not finished, so it was masked until the last range's end had been
  placed; then a cue whose last range was on another slot read silent and ended early, freeing a
  voice that was still sounding.

`tests/fixtures/logs/persistent.wfglog`, which §13.11 planned and nobody wrote, is written with the
fixes: a real hosted session of `bundles/persistent` through a double Esc, the GO after it and a
jump.

### 18.9 The client and the hands

- **Show settings.** An **Inputs** list — name, width, channels and a meter a row — takes the place
  of the typed count of logical channels at the top of the input patch, and the Audio tab's *"live-
  input monitoring and rack processing are not available yet"* goes. A **Rack** tab lists the
  channels: name, class, each chain with Add, Remove, order and preset, each plugin's state in
  words, the worst-case delay against the budget, and Load now.

  *As built (9b.3).* After the Plugins tab, whose scan the chain is made from. The channels on
  the left, two lines a row — the name, then the class and how many plugins, with *over budget*
  in words where it is; *+ Mono*, *+ Mono to stereo* and *+ Stereo* above them are
  `channel.create`; a double click renames, the class is a menu, the cross deletes the channel and
  its chain. The picked channel's chain on the right, two lines a plugin — the name, then its
  state and sentence; *Add…* is a menu of this machine's plugins (grouped by maker past two dozen)
  and sends `channel.plugin`; a row dragged is `object.move` within the channel; *Preset file…*
  and *Restart* act on the picked plugin as on the Plugins tab. At the foot, the picked channel's
  worst case against the budget in one sentence — *Vox 1: 6.7 ms at worst, with every plugin in -
  over the 5 ms budget* — naming any plugin not counted because it has not loaded; the budget
  itself is typed at the top right; *Load now* is lit when the set or the rack differs from the
  graph. The channels are not dragged: the `Rack` element carries no id for `object.move` to name,
  and nothing about the sound depends on their order. The model is `client/model/Rack.h`.
- **The cue list and the inspector.** The new-cue bar gains *Mic*. A mic cue's row wears its mark
  and its input's name where a media cue shows its file. The inspector's input menu lists the
  named inputs with their widths; its channel menu lists the channels with their class and who
  holds each now. Fade-in, level, routing and sends as a media cue's, and the EQ and FX openers.
- **The foot.** `in · Voix solo ▶ [EQ] ▶ [the channel's plugins] ▶ out`, the §17.13 chain with the
  input where the file was, and the budget's words.
- **The running pane.** *Waiting for Vox 1*, *ringing out*.
- **Surfaces.** `surface.aim` accepts a mic cue, so the EQ, Send and FX pages work on it; a DCA strip
  rides a mic cue's DCA as any.
- **The page.** `inspector.js` shows and writes the input, the channel and the fade-in.

### 18.10 Fixtures, drivers, and what the phase measures

**An input for the tests.** `serve --hosted` and `--render` take `--input-wav=<file>`: the file,
loaded whole at open, feeds the logical inputs from its channels, looped. That is how the unit
cases, the replay fixture and the drivers hear a live input on CI, which has no interface.

- **Fixtures.** `bundles/mic` — two named inputs, a rack of *Vox 1* (mono, the test-gain child) and
  *Band* (stereo), a mic cue, a persistent mic; `logs/mic.wfglog` recorded from `serve --hosted
  --input-wav`; `logs/persistent.wfglog` (§18.8).
- **Drivers.** `blackbox/phase9b_inputs.py` (a named input's meter moving on a tone);
  `blackbox/phase9b_mic.py` — a tone in and at the output, the level, the test-gain insert at half,
  a child killed mid-cue leaving it dry with words, Esc ringing out then freeing the channel for a
  waiting cue, double Esc silent at once, a persistent mic kept across a jump and restored by GO
  after a double Esc. Both locales.
- **Measurements.** **M38** — the callback's cost with 0, 4 and 8 rack channels open: the tap's copy,
  the input stages, the chains. **M39** — microphone to output on the author's MADIface through a
  loopback cable, against the words: the rack adds nothing but the interface and what the plugins
  declare. **M40** — the cores spinning with 1, 2 and 4 channels each with a plugin switched in.

### 18.11 What this phase does not build

**Shared rack channels** — a reverb return that media and mic cues send into, §3.9e's *"bus with a
chain"*: they need a return track and Tracktion's aux sends, and stay Phase 9b's next.
**`Media/Insert` made real** (CP). **A cue taking over a held channel**, moving it to its own
settings in its fade time *(proposed)*. **A mic cue as a sampler member**, fader-start for a
microphone *(proposed)*. **Inline hosting**, **AUv3**, **curated parameter maps**, **macOS audio
workgroups for the child** — as §17.10 left them. And the **loop recording** of §19, built next on
this.

### 18.12 What was built, against what §18 drew

*Written at close-out.*

## 19. Phase 9c — live sampling channels: a take, its layers and its loop: what the tree, the commands and the log gain

Written on 2026-09-26 with §18 and before any of it is built; built after §18, on top of it. Rows
reach the CSV with the stage that implements them, and §19.11 at close-out says what won where the
text and the code disagree.

The request was the second half of the author's (§18): *"… live sampling channels that can take in
an input, loop with continuously variable in and out points with pre-recording and
post-looping/playback effects."* It was not in the PRD; PRD §3.31 now carries it.

**What it is, before any of its names.** A rack channel may carry a **recorder**. A mic cue on such
a channel records what its input sounds like through the channel's first plugins — the effects
**before the recorder**, printed into what is recorded — and loops it between an **in** and an
**out** point that move while it plays, through the channel's other plugins — the effects **after
the player**, heard as it loops and changeable without recording again. *Rec* records; *Rec* again
closes the **take** and loops it; *Rec* while it loops lays another pass on top — a **layer** —
and *Undo* takes the last layer off; *Clear* empties the channel. The take is tonight's: it lives in
memory for the session, belongs to the channel rather than the cue, and **Keep** turns it into a
file of the show's media. Cues aimed at it drive it, and so do the hands — the D700's Rec, a Loop
page on its rotaries, the take's picture in the window, the master dial.

**Where it starts, in the code.** Nothing in Go.dot records audio; the only file written from the
audio path is the hosted render's output (`HostedAudioDriver.cpp:36-119`). A **range** (§3.24)
already loops a region of a file, but its points are the clip's loop properties, which are on
Tracktion's restart list (`tracktion_Edit.cpp:146-174, 381-394`): changing one rebuilds the graph,
which is why a range's points are fixed at its arm (`Runner.cpp:6481-6486`). A point that moves
continuously cannot be a clip's, so the looper is a stage of Go.dot's own with its points in
atomics — the EQ's shape (§17.5), not the range's. What a range gave that is reused: the word *in*
and *out*, the waveform drawn from peaks (`peaks::Collector::add`, `Peaks.h:111-125`, which takes a
buffer in memory), and the running pane's way of saying where a loop is.

Four rules hold. **GO never blocks**: a take's memory is set aside when the show opens, never when a
Rec is pressed. **The audio thread is a lipogram**: the looper allocates nothing and, because its
memory was not only set aside but **touched** at open, its first write never faults a page in on
the audio thread. **The document holds what somebody decided** (§4.10): a take is what happened
tonight, not a decision, so it is in no file until somebody keeps it — Keep is the decision.
**Not a DAW** (§1): one take a channel with its layers — no editing, no arrangement, no second take
beside the first.

### 19.1 The decisions (2026-09-26)

Asked with §18's, each with a recommendation; all taken but one.

| | Decision | Whose |
|---|---|---|
| **BZ** | **Two chains**: effects **before the recorder**, printed into the take, and effects **after the player**, heard as it loops | the author's |
| **CA** | **Rec to Rec, with overdub**: Rec again closes the take and loops it; while it loops, Rec lays a new pass on top, **each layer undoable** | the author's — the recommendation was a single take |
| **CB** | **A take lives in memory for the session; Keep writes it into the show's media as a file** a media cue can play | the author's |
| **CC** | **Cues and hands drive it**: transport cues aimed at it, with new verbs beside `advance` and `fade`; the D700's Rec; the window | the author's |
| **CD** | **A Loop page on the D700**: in, out, a rotary sliding both and keeping the length, level; also the take's picture and the master dial. **The loop plays on through every move and every wrap is crossfaded** | the author's |
| **CF** | **GO on a sampling cue does what the cue says**: `wait` (the take silent until Loop or Rec, the default), `loop` the take it finds, or `clear` it for a fresh one | the author's |
| **CQ** | **The loop points are the channel's session state**, not rows of the show: live rows answered in front of the document, logged, never a step of the history, allowed under the lock | implementer's call |
| **CR** | **The recorder is Go.dot's own `Looper`**: memory set aside and touched at open and owned outside the Edit; layers kept apart; wraps and jumps crossfaded inside the take; a loop at least two crossfades long; the EQ after the player | implementer's call |
| **CS** | **Rec never destroys a take** — only Clear empties, and Undo takes one layer; a full take closes itself; the D700's transport Rec cycles; the Loop page on Pan; `through` off by default | implementer's call |
| **CT** | **Keep copies the closed layers mixed**, writes a WAV under `media/takes/` off the message thread, and *Keep as cue* adds a media cue looping it | implementer's call |

**CA — overdub, against the recommendation.** One take replaced by the next Rec was recommended as
less to build and less to explain on the night. The author chose the looper's own instrument: layers
building up, each one undoable. What it costs: a take's memory is multiplied by the layers kept
(§19.2), and a channel that has used them all refuses another pass in words until one is undone or
the take cleared — the alternative, folding the oldest layer into the base while the loop plays,
would be a copy of the whole take on the audio thread or a race with it.

**CF — GO and a take already there.** A take belongs to the channel, so *scene 5 brings back the loop
of scene 2* is a second sampling cue on the same channel finding it. And *open the sampler for a
fresh take* is the same GO wanting the opposite. So the cue says which, and the default is the one
that makes no sound nobody asked for.

### 19.2 The sampling channel

A rack channel with **`takeSeconds`** above nought carries a recorder; **`layers`** says how many
passes on top of the first it keeps. Each of its plugins says its **`side`**: `before` the recorder
or `after` the player, `after` by default, so a plugin added to a channel that had no recorder stays
where it was heard. The chain:

`LiveInputPlugin` → the plugins before → `LooperPlugin` → `EqPlugin` → the plugins after → `CueOutputPlugin`

The EQ sits after the player: it is the cue's EQ on what is heard, not something printed into the
take. With the recorder out of circuit — a mic cue on the channel that records nothing — the looper
passes its input through, and the channel is §18's.

**The memory**: (1 + `layers`) × `takeSeconds` × the rate × two channels × four bytes — sixty seconds
and four layers at 48 kHz is 115 MB — set aside and every page of it touched when the show opens,
never on the audio thread, and said in words in the Rack tab. It is **owned by the audio host beside
the Edit**, one store per channel, and not by the plugin: every media arm rebuilds the graph
(`AudioHost.cpp:1134`), a plugin may allocate only in `initialise`, and Load now and a change of
interface throw the Edit away (`AudioHost.cpp:670`). The store keeps its takes across all three while
the channel's width and the rate are unchanged, and clears them otherwise with a sentence.

### 19.3 The take

| state | Rec | Loop | Undo | Clear |
|---|---|---|---|---|
| **empty** | starts recording | — | — | — |
| **recording** | closes the take and loops it | closes the take and loops it | abandons the recording | empties |
| **looping** | starts a layer | — | takes the top layer off | empties |
| **overdubbing** | closes the layer and loops | closes the layer and loops | abandons the layer being laid | empties |
| **held** — a take, silent | loops it, laying a layer from the in point | loops it | takes the top layer off | empties |

**Rec never destroys a take** (CS): pressed on a channel holding one it adds, it never replaces.
Only Clear empties, and Undo takes one layer at a time — never the first pass, which is the take.
A take reaching `takeSeconds` closes itself and loops, with *the take reached its 60 seconds and was
closed*; a Rec with every layer in use is refused, *Vox 1 holds its 4 layers: Undo one or Clear*.
A layer is as long as the take: it is laid at the playhead, wrapping with it, and a pass longer than
the loop adds into its own beginning, as a looper's overdub does.

**The take belongs to the channel for the session** — a later sampling cue on the channel finds it,
and `onGo` decides what GO does with it (CF). It survives its cue's end, Esc and double Esc: a double
Esc stops everything that sounds and unmakes nothing that was recorded. Clear, the end of the
session, and a change of the channel's width or of the rate end it.

**`through`**, a switch on the mic cue, off by default, adds the input to what the channel sounds
while it records and loops; off, the channel sounds the loop alone, because the voice being sampled
is usually heard through the desk already and would otherwise double.

### 19.4 The loop points

**`loopIn` and `loopOut`** are seconds into the take, on the channel, answered in front of the
document by the take's door — the `LiveRows` pattern (`cue/LiveRows.h`): persisted nowhere, logged
and replayed as the `node.set` they are, never a step of the history, allowed under the lock (CQ).
The door clamps them to the take and keeps them at least two crossfades apart; each time a take
closes they are set to its two ends.

They are not rows of the show because they belong to tonight's take. As rows of the show the engine
would write the document each time a take closed, a night of rides would fill the history, and every
one would ask Keep or Discard at the unlock. A cue that says *loop the first two seconds* is a
proposal, not built.

**The loop plays on through every move** (CD). The playhead carries on; a move that leaves it
outside the loop sends it to the in point, crossfaded. **Every wrap is an equal-power crossfade** of
ten milliseconds taken inside the take, so a point moved onto a loud sample does not click. There is
no *slide* row: the page's third rotary writes both points by the same amount.

### 19.5 The rows

| Node | Type, default | Access | Persist | Meaning |
|---|---|---|---|---|
| `/godot/slot/<id>/takeSeconds` | `d`, 0 s (0..600) | rw | show | the longest take the channel records; nought is no recorder |
| `…/layers` | `i`, 4 (1..16) | rw | show | how many passes it keeps on top of the take |
| `…/take` | `s`, `empty` (`empty\|recording\|looping\|overdubbing\|held`) | r | none | what the take is doing, in a word |
| `…/takeLength` | `d`, 0 s | r | none | how long the take is |
| `…/takeLayers` | `i`, 0 | r | none | how many layers lie on it |
| `…/playhead` | `d`, 0 s | r | none | where the loop is playing, in the take |
| `…/loopIn`, `…/loopOut` | `d`, 0 s | rw | none | the loop, seconds into the take — the take's door (§19.4) |
| `…/takeProblem` | `s` | r | none | the last thing the take refused or did by itself, in a sentence |
| `/godot/plugin/<id>/side` | `s`, `after` (`before\|after`) | rw | show | on a sampling channel's plugin: before the recorder or after the player |
| `/godot/cue/<id>/onGo` | `s`, `wait` (`wait\|loop\|clear`) | rw | show | on a mic cue: what GO does with a take the channel already holds (CF) |
| `…/through` | `T`, false | rw | show | on a mic cue: hear the input as well as the loop |

`transport/verb` gains `record`, `loop`, `overdub` and `clear`.

### 19.6 The commands

| Command | Arguments | What it does, and what it records | What it refuses |
|---|---|---|---|
| `take.record` | `<channel s>` | the Rec press, §19.3's table | no sampling cue holding the channel — `not-running`; every layer in use — `layers-full`; a Keep copying — `busy` |
| `take.loop` | `<channel s>` | the Loop press | `not-running` |
| `take.overdub` | `<channel s>` | a layer begun while it loops or is held, or closed while one is laid — what Rec's cycle does, said explicitly for a cue | `not-running`, `layers-full` |
| `take.undo` | `<channel s>` | the top layer off, or the pass being laid abandoned | `busy` |
| `take.clear` | `<channel s>` | the channel empty | `busy` |
| `take.keep` | `<channel s> [asCue T]` | §19.8 | a take being recorded — `not-closed`; `asCue` under the lock — `locked` |
| `take.closed` | `<channel s> <samples i>` | engine origin: the length the audio thread closed a take at — full or pressed — so that a replay knows it | — |
| `take.kept` | `<channel s> <file s> <error s>` | engine origin: the file Keep wrote, or why it could not | — |

**Where a press lands in time.** The handler moves the take's state in the model; a hook places the
start of a recording, the close of a take and the start of a layer at the sample `now + the launch
latency`, as a launch is placed (`Runner.cpp:6299`), so a press and a cue land on a sample the log
can name and a replay places the same.

**Transport cues** aimed at a mic cue send `record`, `loop`, `overdub` and `clear` to its channel,
handled in `fireStop` before the hard stop it falls through to today (`Runner.cpp:3217-3230`). A
target that is not a sounding sampling cue is applied and does nothing — the transport cue's own
rule for a target that is not running.

**GO** (`onGo`): `wait` holds the take silent; `loop` loops it from the in point, and with no take
waits for Rec; `clear` empties the channel and waits for Rec.

### 19.7 The hands

- **The D700.** The transport's **Rec** (`0x5F`, `Button::record` — not a strip's REC, which sets a
  start level, `SurfaceProfile.cpp:121-122`) is `take.record` on the aimed sampling cue's channel; its
  double click, if the firmware gives that button one, `take.undo`. A **Loop page on Pan** (`0x2A`,
  unbound: Go.dot has no pan) puts the aimed sampling cue on the rotaries — in, out, a third sliding
  both and keeping the length, and the level — with a law fine enough for a loop point, the first
  thing the bench says. A **Loop** key, if the D700 has one (its protocol note lists Rec, Play and
  Stop), is `take.loop`. Under the lock the page writes as it always does: the points are not the
  document.
- **The window.** A sampling cue's foot shows the take's picture, drawn from peaks the looper keeps a
  block at a time and the desktop reads in its own process, growing as it records; the in and out as
  edges to drag; the playhead; the layers; and Rec, Loop, Overdub, Undo, Clear and Keep. The chain
  above it is §18's with the recorder in its place: `in ▶ [before] ▶ ● take ▶ [EQ] ▶ [after] ▶ out`.
- **The master dial.** Rule BQ (§17.18) is amended: a live row of a sampling channel is a number a
  hand may write, and the dial turns it.

### 19.8 Keep

`take.keep <channel> [asCue]`: the closed layers — the take and every layer on it, summed at unity,
the whole take and not only the loop — copied from what the audio thread has finished writing (a pass
being laid is not in it), written as a WAV at the session's rate under `media/takes/`, atomically, by
a job off the message thread, and recorded with `take.kept`. With `asCue`, a media cue after the
sampling cue plays the file, with a Range at the loop points set to loop; under the lock it is
refused, as any change of the show. While the copy runs, Undo and Clear wait, refused `busy`, so the
file is the take that was asked for.

### 19.9 Fixtures, drivers, and what the phase measures

- **Unit.** `tests/LooperTests.cpp`: a take recorded and closed to the sample; Undo bit-exact; the
  largest sample-to-sample step at a wrap on a sine; points moved mid-loop; a full take closing
  itself; the whole of it under rtsan.
- **Replay and driver.** `logs/take.wfglog` and `blackbox/phase9c_take.py`, with `--input-wav`: Rec,
  Rec and the loop heard; a layer summed; Undo; points moved with no click; Keep, a file, and a media
  cue looping it.
- **Measurements.** **M41** — a take's memory and the time to set it aside and touch it at open, at 60
  seconds and 4 layers, 48 and 96 kHz. **M42** — a block's cost by layers, 1 to 16. **M43** — the
  wrap: the largest step at the join on a sine, against the same sine unbroken. **M44** — where a take
  starts against where Rec was pressed, from a recorded click.

### 19.10 What this phase does not build

Overdub feedback, a decay applied to every pass; varispeed and reverse on a loop — the varispeed
the author asked for on files on 2026-09-21 is the same conversation; more than one take on a
channel; editing a take; loop points written in the show; a sampling cue on a sampler strip; the
page's take panel.

### 19.11 What was built, against what §19 drew

*Written at close-out.*
