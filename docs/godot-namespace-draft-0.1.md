# Go.dot — Parameter-tree namespace and document schema

**Draft 0.3** — the PRD §9.3 "parameter-tree namespace and node metadata schema". It also
fixes the show-document schema, because the document is the other half of the same
namespace: every node under `/godot/cue` is a projection of an attribute in `show.xml`.
Draft 0.3 adds §11, the shape Phase 2 gives the tree, written ahead of its code so that the
Phase 2 pull requests have something to be reviewed against.

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
| `/godot/list/order` | `s` | ro | list IDs in order, space-separated |
| `/godot/list/focus` | `s` | rw | ID of the focused list; exactly one; write = `list.focus` |
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
- **`/godot/list/order` and `/godot/list/focus` are NOT built.** They need a parameter-table
  row for the `/godot/list` container itself, which needs a new owner token, a containment
  entry, container-level address resolution and a root-attribute case in both the state
  writer and the RELAX NG generator. The author settled focus as **runtime only** for Phase 1
  (2026-09-06) — the smallest thing that makes `standby.next` unambiguous — because the
  development plan puts the focus model in Phase 3, with parallel lists. Focus is therefore
  engine state, resolved rather than stored: the requested list if it still exists, otherwise
  the first list. Nothing has to maintain it, and "exactly one list is focused whenever a
  list exists" is true by construction. **Phase 3 publishes both nodes**; until then a client
  cannot read or write the focus, and the roster is only visible as the shape of the tree.
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

**Not built yet, and noted so it is not mistaken for an oversight:** `/godot/list/order` and
`/godot/list/focus` in §2.3 have no rows in the parameter table, and a table row is what
makes a node exist. Both belong with the cue list in PR 1.7, which is also where `focus`
acquires a meaning.

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

### Open, with the subphase that forces each

| # | Question | Forced by | Fallback if undecided |
|---|---|---|---|
| E | Does the Phase 5 desktop UI run **in-process or as a separate client**? | Phase 5, but it shapes Phase 2's plugin-parameter handover | assume separate, because that is the stricter assumption and the one PRD §3.2 reads most naturally |
| J | **Should PRD §4.2 record what Tracktion does inside the callback?** Its device callback takes one uncontended `std::shared_lock` per block and its node-player pool uses semaphores; the lipogram can be *enforced* on Go.dot's code and only *measured* on Tracktion's (§11.5). A PRD amendment is the author's to make. | the lipogram test (PR 2.2) | enforce on Go.dot's scopes, report Tracktion's count separately, never hide it |
| K | **How does a mount declare what it can do?** `transport` says how to *send* and nothing says whether the target can be *asked*, so `wait: verified` against a write-only device is a cue that cannot succeed and nothing notices until the show. Chataigne carries two booleans per module, `hasInput` and `hasOutput`, for exactly this. Also: whether the answer names the *mechanism* (`oscquery` \| `poll` \| `subscribe` \| `none`) or only the capability. | `verified` (PR 2.6) | a mount-level `readback` enum defaulting to `none`, and a `verified` cue against `none` refused at load — the strictest reading, and the one that cannot fail silently |

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
Phase 3), and mounts stop being stubs in Phase 2. §11 draws the Phase 2 part.

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
