# The parameter table

`godot-parameters.csv` is the list of everything Go.dot exposes, and it is meant
to be **added to as the engine grows** rather than written out in advance. The
project is a work in progress; the table is the place that admits it.

One file, four surfaces. Every row generates all of:

| Surface | What the row gives it |
|---|---|
| the show document schema | the element, its attribute, its type, its default, whether it is persisted at all |
| the parameter tree | the node, its type, its access, and the four things PRD §3.3 says a node declares |
| the RELAX NG schema | the attribute's datatype, its enumerated values or its range as a facet |
| the OSCQuery reply | `TYPE`, `ACCESS`, `RANGE`, `DESCRIPTION`, and the `GODOT` metadata key |

**Why one file rather than four.** WFS-DIY keeps three of these independently and
reconciles them with a runtime auditor that logs drift after the fact — a
`MCPOSCQueryAuditor` exists precisely because the CSV, the hand-written OSC
router and the OSCQuery namespace builders can disagree. spatcore's own boundary
proposal recommends collapsing them, and Go.dot is early enough to simply start
that way. If a parameter is not in this file, it does not exist in any of the
four places; if it is, it exists in all of them, spelled the same.

## Columns

| Column | Meaning |
|---|---|
| `owner` | which object carries it: `engine`, `document`, `list`, `cue`, `group`, `mount`. A `group` is a cue, so it also gets every `cue` row. |
| `address` | the node's address under its owner. `/godot/cue/<UID>/name` comes from `owner=cue, address=name`. |
| `type` | one OSC type tag: `s i h f d T b`. `T` means a boolean, which also accepts an int 0 or 1 because many senders cannot emit `T`/`F`. |
| `access` | `r`, `w` or `rw`. A write to an `r` node is refused and logged, never ignored. Independent of `persist`: a mount's prefix is stored in the file *and* read-only at runtime. Conflating the two was a real bug here. |
| `default` | omitted from the document when the value equals it, which is what keeps the canonical form sparse. |
| `range` | `min..max` (either end may be empty) or an enum as `a\|b\|c`. Blank means unconstrained. |
| `unit` | `s`, `Hz`, `dB`, `samples`, `m`, … Blank when the value is not a quantity. |
| `kind` | `state` or `event`. An event is one-shot and has no value at a given time (PRD §3.3), so it is never persisted and never solved for. |
| `rate_cap` | Hz, the cap on outbound dispatch for this node. |
| `anticipatable` | whether it may be pre-sent before GO: imperceptible *and* revocable. Third-party defaults to `no`. |
| `panic` | its defined resting state (PRD §4.6): `park`, `snap`, or a literal value. |
| `persist` | `show` (someone decided it), `state` (the machine happened to be doing it), or `none` (runtime only, never written). It is also **which file the attribute lands in**: `show` goes to `show.xml` and `state` to `state.xml`, enforced in both directions — the canonical writer will not put a `state` attribute in the document, and the document reader refuses to read one, saying which file it belongs in. This column is PRD §4.10 made mechanical, and it is also how a DERIVED value is marked — a cue's `index`, its `kind`, a list's `order` are computed from the tree, so storing them would be a second copy that eventually disagrees. A `none` row is not a document attribute at all. |
| `description` | one sentence, and it is the `DESCRIPTION` an OSCQuery client shows. Write it for the person reading it at 2 a.m. |

RFC 4180 CSV: comma-separated, and a field containing a comma or a quote is
double-quoted. Editable in a spreadsheet, which is the point — but note that
`description` is prose and will often need quoting, so check the diff after a
spreadsheet round trip.

## Adding a parameter

1. Add the row. Keep rows grouped by owner, in the order a person would read
   them, not alphabetically.
2. Nothing else, if the parameter is an ordinary value: the schema, the tree, the
   RELAX NG and the OSCQuery reply all follow.
3. If it needs behaviour — a range that depends on another value, an invariant to
   maintain — that is a command handler, and the row still comes first.

## What is not in here

Cue *content* — media, fades, outputs, bindings — is Phase 2 and later. The table
starts with the skeleton Phase 1 needs and grows a block at a time as each phase
lands. Its shape is expected to change while it is small; it is the wrong file to
be precious about.
