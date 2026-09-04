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

### 2.4 `/godot/cue` — every cue and every group, flat, by ID

| Node | Type | Access | Meaning |
|---|---|---|---|
| `/godot/cue/<id>/kind` | `s` | ro | `memo` \| `group` (Phase 1 has no media; `memo` is a cue with no output) |
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
| `/godot/cue/<id>/preWait` | `d` | rw | seconds |
| `/godot/cue/<id>/postWait` | `d` | rw | seconds |

Groups carry no outputs, media or parameters (§4.12); Phase 1 stores their structure and
attributes so fixtures can nest, and nothing more. question C in §9 fixes what
`standby.next` does with them.

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

**Rejection rules** (fixed, because the log records outcomes): unknown command, unknown
id, bad address, read-only node, type mismatch other than `i`↔`f`, `standby.set` outside
the focused list, a retired id offered to `create` → the event is rejected, logged as `R`
with a reason code, and surfaced at `/godot/engine/lastError`.

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

### 6.2 `show.xml`

Attributes come from [`parameters/godot-parameters.csv`](parameters/godot-parameters.csv)
— the rows whose `persist` column is `show`. The table below is that file read back as a
grammar, and it grows with it.

| Element | Attributes (type, default) | Children |
|---|---|---|
| `Show` | `formatVersion` int (1) | `Lists`, `Mounts` |
| `Lists` | — | `List*` |
| `List` | `id`, `name` string | `(Cue \| Group)*` |
| `Cue` | `id`; `number` string; `name` string; `kind` enum (`memo`); `notes` string; `enabled` bool (true); `colour` string; `preWait`, `postWait` double seconds (0, cap 6) | — |
| `Group` | `id`; `number`; `name`; `notes`; `enabled`; `colour`; `mode` enum (`sequence`); `advance` enum (`manual`); `preWait`, `postWait` | `(Cue \| Group)*` |
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

### 6.3 `state.xml` (question A)

```xml
<State formatVersion="1">
  <Focus list="7K2QM9X4"/>
  <Standby cue="B3N8R5TW" list="7K2QM9X4"/>
</State>
```

### 6.4 `MyShow.wfg`

```xml
<Bundle formatVersion="1"/>
```

### 6.5 RELAX NG

`docs/schema/show.rng` is generated from the engine's `Schema` table by `wfg schema` and
committed; CI fails if the committed file differs from the generated one.
`scripts/validate-show.py` (lxml) validates any bundle and is the pre-commit hook §3.20
asks for.

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
X 13 4 udp:192.168.1.7:9000 malformed-packet b:LyIvAAAsZgAA
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

- **Identifiers — 8-character Crockford base32.** `[0-9A-HJKMNP-TV-Z]{8}`, 40 bits from
  `std::random_device`, unique within the document at creation.

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

- **I — Audio backends as WFS-DIY builds them** (settled 2026-09-05): `JUCE_ASIO=1` behind a
  `WFG_ASIO_SDK` path variable (the SDK is not redistributable, so without a path the build
  is WASAPI/DirectSound only) and `JUCE_JACK=1` on Linux with `libjack-jackd2-dev` in the
  package list. CoreAudio needs nothing.

### Open, with the subphase that forces each

| # | Question | Forced by | Fallback if undecided |
|---|---|---|---|
| A | Does **standby survive save/load**, or start empty every time? §3.20 lists ephemeral state without naming standby; §3.5 calls it engine state. | the cue list (PR 1.7) | persist it in `state.xml` — a rehearsal reopened where it was left is the kinder default, and it is one row of the CSV to reverse |
| C | Does `standby.next` **descend into a group**, or step over it? §3.6 says the pointer descends into a manual sequence group; §3.5 says it lands after an automatic chain. Both are Phase 3 behaviour. | the cue list (PR 1.7) | step over it in Phase 1 and say so in the test, rather than implement half of Phase 3 |
| D | The **touch-gating** vocabulary (§3.16 "required from day one"): `node.touch` / `node.release` per origin, pushes suppressed to the touching origin, released on disconnect. | the OSCQuery server (PR 1.9) | as described — it is the smallest thing that satisfies §3.16, and no surface exists yet to disagree with it |
| E | Does the Phase 5 desktop UI run **in-process or as a separate client**? | Phase 5, but it shapes Phase 2's plugin-parameter handover | assume separate, because that is the stricter assumption and the one PRD §3.2 reads most naturally |
| F | The **mount** attribute set: `transport` declared now and used from Phase 2, a mount-level `panic` default with per-node overrides. | mounts (PR 1.6) | as drawn in §2.5 |
| J | **Should PRD §4.2 record what Tracktion does inside the callback?** Its device callback takes one uncontended `std::shared_lock` per block and its node-player pool uses semaphores; the lipogram can be *enforced* on Go.dot's code and only *measured* on Tracktion's (§11.5). A PRD amendment is the author's to make. | the lipogram test (PR 2.2) | enforce on Go.dot's scopes, report Tracktion's count separately, never hide it |

None of these blocks the next subphase, and the one that was due soonest — B, tombstones —
is now settled. J changes no code either way; it changes what §4.2 claims.

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
