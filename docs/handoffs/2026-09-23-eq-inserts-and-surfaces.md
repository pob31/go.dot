# EQ and VST inserts, and what the surfaces will want from them — 2026-09-23

For the session that implements the **per-cue EQ and VST inserts**. The author has decided
that each media cue gets **an EQ and one or more VST inserts**, and that the **rotaries of the
control surfaces** should reach their parameters. The inserts come first; the surface pages
follow on top of them. The design for the pages is
[`docs/godot-surface-pages-draft-0.1.md`](../godot-surface-pages-draft-0.1.md), and **its §8 is
the part addressed to you**.

## Start from `phase6`, not from `main`

*Superseded the same evening: `phase6` was fast-forwarded onto `main` at `c3d75fe` and the
EQ/inserts session works on `main` as **Phase 9a** — plan decisions AD–AG in
`docs/godot-namespace-draft-0.1.md` §9, the phase drawn as §17. The author's answers to this
note's two open questions: the EQ is Go.dot's own, and the inserts are a chain on every voice
track — the show's plugin set, every voice carrying it — behind the out-of-process proxy, built
first. The file list below stays true as a list of what Phase 9a touches.*

**Phase 6 is on the local branch `phase6` and is neither pushed nor merged** (commits `f43d104`
to `9a4dbd6` and the one that added this note). Work begun on `main` will collide with it in
exactly the files an EQ and inserts will touch:

- **the parameter table** (`docs/parameters/godot-parameters.csv`) — Phase 6 added about fifty
  rows, the media cue's sampler rows and `initialLevel` among them — and so the generated
  `SchemaTable.generated.h` and `docs/schema/show.rng`;
- **the containment table** (`Schema.cpp`) and **`ShowDocument`** — `<Surfaces>` and `<Dcas>`;
- **every fixture show** — each gained `<Surfaces/>` and `<Dcas/>`;
- **`Runner.cpp`** — a run's level is now its own level, plus its trim, plus its DCA chain, plus
  every ancestor group's (`Runner::applyLevels`);
- **`ParameterTree.cpp`** and **`Console.cpp`** (the surface bridge's serve wiring);
- **the inspectors** — the media rows' order in `client/model/Inspector.cpp` (`kindOrder`) and
  `clients/console/views/inspector.js` (`KIND_ORDER`), where EQ and insert rows will want a place.

Branch from `phase6` (or merge it first). Namespace draft §16 describes everything it built;
§16.12 says where the build departed from the drawing.

## What the surfaces will need — §8 of the design, in short

1. **Every EQ and insert parameter a node** under its cue, carrying its value and published with
   its name, short name, default, range or steps, whether it is bipolar, and the plugin's own value
   text when an instance exists.
2. **Written with `node.set`**, from any origin — so a surface, the desktop and the page all reach
   it, the touch table gates it, and undo's coalescing (same address, same origin, under half a
   second) makes one turn one step. The hop to the message thread, which Tracktion demands for
   plugin parameters (PRD §3.18, measured at about 4 µs a write), is the engine's and never a
   client's.
3. **The insert order readable** — which inserts a cue has, in order, with each plugin's name.
4. **Names and ranges readable without a playing instance** where the scan can give them.

None of this asks for a page, a surface change or a bridge change.

## Two engine facts the design leans on

- **A track's plugin chain is structural**: Tracktion restarts playback when it changes, so a cue
  cannot insert its plugins at the moment it plays; bypass restarts nothing (PRD §3.18, verified
  against the pinned engine on 2026-09-07). Whether per-cue inserts live in **a fixed chain on
  every track** or in **rack channels a cue claims** is the author's open question 8 in the design.
- Go.dot's **own output plugin sits at the end of every track**, carrying the level and the
  routing matrix; an EQ and inserts belong before it.

## Working conventions that bit Phase 6

- **The CSV is the single source of truth**: edit it, run `python scripts/generate-schema.py`, build,
  then `wfg schema --out=docs/schema/show.rng`. `ctest -R schema` catches a step left out.
- A `persist=none` row is **derived by construction** and refused `read-only` at the document's
  door; a writable runtime row needs a door in front of the document (`cue::liveWriteFor`, §16.4).
- The Linux CI builds with GCC `-Werror -Wshadow -Wsign-conversion -Wfloat-equal`; there is no local
  GCC, and a clang-tidy diagnostics pass with the same flags is the stand-in.
- Several working-tree files are CRLF (`Console.cpp`, `tests/CMakeLists.txt`, many tests); scripts
  that edit them keep each file's own line endings.
