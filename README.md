# Go.dot

A cross-platform show control application for theatre, dance and live music.

## Project Overview

Go.dot is a cue list that carries audio and video, holds live parameter
bindings during a show, and acts as the conductor for a family of specialised
processors (WFS-DIY, XOA/Tight-WFS, S21-HiJack) that it commands but does not
contain.

Three ancestors, one synthesis: **QLab** for the cue list, fades, chaining and
operator ergonomics; **Ableton Live** for hands on parameters while material
runs (the binding layer, not clip launching); **Chataigne** for computation on
protocol streams — the control-rate dataflow graph. The engine is headless and
every client, including the desktop UI, is an equal peer over OSCQuery. Audio
playback is [Tracktion Engine](https://github.com/Tracktion/tracktion_engine):
Go.dot owns time, TE is a sample-accurate polyphonic player with a plugin graph.

It is **not** a DAW, not a lighting console, and not a spatial renderer.

The specification lives in `docs/`, and it is the spec — not background reading:

- **[`docs/godot-prd-draft-0.8.md`](docs/godot-prd-draft-0.8.md)** — Product
  Requirements. Anything marked *(proposed)* in it is a design put forward and
  **not yet confirmed**; it does not get built without an explicit yes.
- **[`docs/godot-devplan-draft-0.1.md`](docs/godot-devplan-draft-0.1.md)** — the
  phase plan. Each phase ends with something runnable.

**To try it rather than build it**, read the tester's guide —
[English](docs/Go.dot_TryItOut_English.md) or
[français](docs/Go.dot_TryItOut_Français.md) — and download a test build from
the [releases page](https://github.com/pob31/go.dot/releases).

---

## Status: test builds, not yet for a performance

Go.dot runs shows. It plays sound and pictures from a cue list, takes GO from
the keyboard, a control surface or the network, and stops in the three ways
PRD §4.4 asks for. Test builds for Windows, macOS and Linux are published from
version tags (the first, v0.1.0, on 2026-09-29). They are for rehearsal rooms
and for people's own machines, to try and to report on — **not yet for opening
night**.

Much of what touches hardware is built and tested in software only. The devplan
ends each piece of work with what is still **owed to the bench** — the D700 on
the unit, a projector on a wall, an Arduino on a serial port, a rate change on
the Dante under a running show — and none of those counts as done until someone
has stood in front of the thing.

### What it does today

- **The cue list and GO.** Cues that play a sound or a picture, fade something,
  send OSC or MIDI, open a microphone, run a Pure Data patch, or enable, disable
  or jump to another cue. Waits at both ends; groups that play as a timeline or
  a sequence, automatic or manual, with headers and footers that always run;
  shuffled rounds with a seed, so a random scene can be rehearsed; parallel
  lists; triggers from OSC, MIDI and the wall clock; ranges and loops inside a
  sound, with loop points that can move while it plays. Two GOs closer than half
  a second are taken as one bounce of the finger.
- **Ready before the hand comes down.** Park the pointer on a scene and the scene
  gets itself ready: its sounds read from disk, the processor inputs it needs
  claimed, the values the desk should hold sent and checked. Move the pointer
  away and all of it is given back. Two cues that want the same input wait
  rather than fight, and the show warns about the overlap while you edit. Jump
  into the middle of an act and the right cues start at the right offsets.
- **Editing that cannot lose a show.** Undo, a save that is never half-written,
  crash-safe autosave with recovery, and an edit lock for the performance that
  leaves GO and the stops working. A Show is the piece and each night is a
  Performance, a copy of the show or of an earlier night, with a template cue
  list to carry good changes back. Save as copies the sounds only when the copy
  would not find them. A `.wfg` file opens in Go.dot on all three systems, and
  with nothing given the last show reopens.
- **Two ways in.** A desktop window, compiled into the same process as the
  engine, which the launchers open; and a web console the engine serves at
  `/ui`, plain HTML and ES modules with no build step, kept as the redundancy
  path. Both change the show only through the engine's named commands, as a
  hardware surface does. The show settings hold audio, outputs, inputs and the
  rack, network devices, MIDI, surfaces, serial ports, video and playback.
- **Sound.** Media cues play through Tracktion Engine, each with its own EQ and a
  chain of VST3, AU or LV2 plugins run in a separate process — a crashing plugin
  silences its cue, never the show, and never leaves it playing dry. A cue's
  level and each of its sends can follow a curve drawn over its waveform or
  recorded from a fader. Speed from nought to twenty, as varispeed or
  timestretch, faded while it plays, backwards or bouncing between its loop
  points. A fade moves anything the cue it aims at owns: level, sends, EQ,
  plugin values. When the interface disappears the show pauses; when its rate
  changes the show follows it and resamples.
- **Live inputs.** Mic cues on named rack channels, through plugins; and live
  sampling — a take recorded during the show, then looped, layered or cleared.
- **Control surfaces.** Mackie Control, and the Asparion D700 in full: sampler
  groups on the strips with fader-start, DCAs, EQ, Send and plugin pages on the
  rotaries, a master dial, standby on the arrows, and the faders flipped onto
  one cue to record its level and sends in one pass. A virtual surface panel
  plays the same groups with a mouse.
- **Video.** Canvases shown on outputs; fills, masks, stills and HAP movies with
  their sound locked to them; placement, grade, curves and blends; several
  canvases on one output, each bent onto a wall that is not flat by a mesh, and
  a CDL per output to match projectors. Pictures are drawn by a separate process
  on Direct3D 11, Metal or OpenGL, which Go.dot restarts if it dies, so a slow
  movie cannot hold up a sound cue. Outputs sent to other programs over NDI,
  Spout or Syphon, live inputs taken from them, and inserts that pass a cue's
  picture through another program and back. Pictures in sampler groups, and
  the next GO's pictures read ahead. A movie in another format plays as a
  preview while FFmpeg (downloaded the first time it is needed) converts it to
  HAP.
- **OSC.** Network devices with an address, ports and receive and send switches;
  OSC cues carrying several messages, sent as one bundle per device; curves
  played on the cue's clock and recorded from what a device reports or from a
  SpaceMouse; a monitor of every message in and out.
- **Process cues.** A Pure Data patch inside a cue, running while the cue runs at
  control rate and never in the audio path. It hears device reports, OSC, MIDI
  and serial lines, and sends to devices, to MIDI and to Go.dot's own commands.
  It is drawn and played on a canvas at the foot of the window, or opened in Pure
  Data or plugdata. Go.dot ships ready-made patches for the usual chores
  ([`pd/`](pd/)) and an example show. Serial ports carry an Arduino's lines, or
  OSC over SLIP.
- **Stopping.** Esc fades running cues out over the panic fade and runs their
  footers, as if they had ended; a persistent cue is paused instead, and the
  next GO resumes it. A double Esc stops everything Go.dot started, at once,
  footers skipped. **Doh!** takes back a GO pressed too early: it stops what
  that GO started, puts the standby back, and can send each device the command
  that undoes what it was sent.
- **Importing.** Ableton Live sets (`wfg import-als`, several sets making a tour
  with a performance for each venue) and QLab 4 and 5 workspaces
  (`wfg import-qlab`), both also in the window's File menu.

### Not built yet

- **Timecode** — chasing and generating LTC and MTC (devplan Phase 10).
- **Panic values** — every parameter declares a resting state (PRD §3.3), but
  nothing yet puts a node back to it.
- **Bindings** — a general way to bind any parameter to any control, with read,
  touch, latch and write modes (PRD §3.10). Today a surface reaches parameters
  through Go.dot's own pages.
- **The tablet** (Phase 7) — the console runs in a tablet's browser, but as a
  mouse-and-keyboard page, without the touch gestures PRD §3.17 describes.
- **Group joins** — gap, gapless or crossfade between a sequence's members,
  decided (PRD §3.6) and waiting for the client's layout to settle.
- **Video latency offsets and DeckLink output** (Phase 8b).
- **Integrations and redundancy** — Choufleur, authoring a cue from a processor,
  device templates, and a backup engine that takes over from the primary
  (Phases 11 and 12).

---

## How it is checked

**It is measured rather than asserted**, which is the part worth reading before
the code.

- **Each phase's done-when clause is a program.** [`tests/blackbox/`](tests/blackbox/)
  holds Python drivers — standard library only, nothing of Go.dot's imported —
  that start the shipped `wfg`, drive it over UDP, HTTP and WebSocket the way a
  console would, and read back what came out, sometimes off the WAV it rendered.
  They keep finding what no unit test can: faults at the seam between two things
  that were each correct alone.
- **Every session can be replayed.** Every input becomes a named command applied
  on a 50 Hz tick and written to a tick-indexed log, and `wfg replay` must
  reproduce a session record for record. A replay fixture per phase runs in CI.
- **Every serialising test runs twice**, under `C` and under `fr_FR`, because a
  number written with a decimal comma is a premiere-night bug.
- **Measurements, numbered M1 to M59 so far**, report rather than gate: where a
  launch lands to the sample, how closely a rendered fade follows its curve, what
  a D700 refresh costs a tick, how long a Pd patch takes. They sit in the
  namespace draft beside the design they tested, and several changed a decision.
- **CI** builds and tests on Linux, macOS and Windows under both locales, with a
  strict `-Werror` build, the spikes, a pin gate, and a job running Clang's
  real-time sanitizer, the second net under PRD §4.2's rule that the audio
  thread never allocates, locks or calls the system.

---

## How it got here

The order things were built, which is not the devplan's numbering: Phase 9 came
before 7 and 8, and several items were brought forward from Phases 10 and 11 at
the author's direction. Each was drawn in the namespace draft before its code
and says afterwards what was built against what was drawn.

| Work | What it added | Written up in |
|---|---|---|
| Phase 0 | A build on three platforms; seven Tracktion Engine spikes, all passing | [`docs/spikes/`](docs/spikes/) |
| Phase 1 | The headless engine: show document, parameter tree, 50 Hz clock, cue list, OSC, OSCQuery, the event log and its replay | namespace draft §1–§8 |
| Phase 2 | First sound: Tracktion hosted, GO, fades, network cues that wait for an answer | §11, [Phase 2 close-out](docs/godot-phase2-closeout-0.1.md) |
| Phase 3 | Groups, triggers, ranges, MIDI cues | §12, [Phase 3 close-out](docs/godot-phase3-closeout-0.1.md) |
| Phase 4 | Prepare and commit, the shared allocator, the state solver, load-to-time | §13, [Phase 4 close-out](docs/godot-phase4-closeout-0.1.md) |
| Phase 5 | Undo, crash-safe save, the edit lock, spectral colour; the console as operator client, then the desktop window | §14 |
| Show settings | Network devices and MIDI ports | §15 |
| Phase 6 | Surfaces, strips, DCAs, sampler groups, the D700 | §16 |
| Phase 9 | Per-cue EQ, the plugin sandbox and inserts; the live rack; live sampling | §17–§19 |
| Lanes and speed | Level and send lanes, recorded from faders; varispeed and timestretch; fades on what a cue owns | §20, §22, §26, §28, §34 |
| Phase 10, in part | The stop levels held to PRD §4.4, the panic fade and GO debounce, Doh! | §21, §23, §24 |
| Shows | Shows and performances, Enable/Disable/Jump, Save as | §25, §27, §32 |
| Imports | Ableton Live and QLab | §29, §46 |
| Phase 8 | Video: stills, movies, the native renderer, NDI/Spout/Syphon, read-ahead, pictures in sampler groups, the DCA knob | §35–§37, §40, §44, §47–§50 |
| OSC cues | Several messages, bundles, recorded curves | §45 |
| Process cues | Pure Data inside a cue, serial ports | §51 |

The documents, besides the PRD and the devplan:

- **[`docs/godot-namespace-draft-0.1.md`](docs/godot-namespace-draft-0.1.md)** — the
  *shape* of the `/godot` namespace and the show document: how a node is addressed, what
  metadata it carries, how a mutation happens and how it is recorded. A living document,
  because Go.dot is not a port of something that already works and there is no finished
  parameter list to transcribe. Its §9 records every decision taken with the author, by
  letter, and its later sections are the table above.
- **[`docs/parameters/godot-parameters.csv`](docs/parameters/godot-parameters.csv)** —
  *what* exists, added to as each piece of work lands. One table generating four surfaces:
  the document schema, the parameter tree, the RELAX NG schema and the OSCQuery reply.
  WFS-DIY keeps three of those independently and reconciles them with a runtime drift
  auditor; starting collapsed is cheaper than collapsing later.
- **[`docs/godot-reuse-map-0.1.md`](docs/godot-reuse-map-0.1.md)** — what WFS-DIY,
  spatcore and juce_simpleweb already provide, per phase, and what stops each piece being
  used as-is.
- **The close-outs** of Phases 2, 3 and 4 — the PRD amendments each proposed, what it
  deliberately left undone, what it measured, and what it changed its mind about.
- **[`docs/godot-open-questions-0.1.md`](docs/godot-open-questions-0.1.md)** — questions
  that are the author's, written down rather than guessed.
- **The D700** — [`docs/godot-asparion-d700-protocol-0.1.md`](docs/godot-asparion-d700-protocol-0.1.md),
  its protocol as measured on the unit, with the byte tables in
  [`docs/D700_CONTROL_GUIDE.md`](docs/D700_CONTROL_GUIDE.md); and
  [`docs/godot-surface-pages-draft-0.1.md`](docs/godot-surface-pages-draft-0.1.md), its
  buttons as Go.dot's pages.
- **QLab** — [`docs/godot-qlab-import-0.1.md`](docs/godot-qlab-import-0.1.md), the
  importer, and [`docs/godot-qlab-extraction-0.1.md`](docs/godot-qlab-extraction-0.1.md),
  what a real workspace held.
- **[`docs/handoffs/`](docs/handoffs/)** — notes passed between pieces of work, the
  hardware checklists among them.

---

## Inside the engine

- **One road in, one ordered path out.** `Engine::submit` takes a command from any thread;
  `processTick` applies every event in arrival order on the tick thread; a named-command
  registry says what exists; and a tick-indexed event log lets a session be replayed and
  reproduce itself record for record. Built before there was anything to record, which is
  the only order in which that guarantee is cheap.
- **`osc::Value`**, the OSC 1.1 value type the whole control plane shares, with a number
  formatter that writes the shortest text reading back as the identical value. Measured:
  JUCE's own writer loses 46% of random doubles to a save-and-load round trip, which is what
  put the macOS floor at 13.3.
- **The show document and the bundle it lives in.** A show is a folder: a manifest, a
  canonical `show.xml` holding what someone decided, a `state.xml` holding where the engine
  had got to, its media, and the OSCQuery descriptions its devices read. Which of the two
  files an attribute lands in is the parameter table's `persist` column and nothing else,
  enforced in both directions. Nothing in a bundle records when or where it was written, so
  opening one and saving it again produces the same bytes — which is what lets a replay
  compare against a saved show directly instead of through a normaliser.
- **[`docs/schema/show.rng`](docs/schema/show.rng)**, the bundle's grammar in RELAX NG,
  generated from the parameter table and committed. The engine validating a document against
  its own schema can only prove it is self-consistent; this is what lets somebody else's
  validator have an opinion, and it is what anyone can run against their own show file
  without building the engine.
- **The 50 Hz tick** (PRD §3.4). One sample counter that never goes backwards, a tick
  index derived from it by exact integer division, and a thread of its own that waits on
  the counter rather than on the wall clock. It never skips a tick, because the index is
  the event log's ordering key; it measures its own lateness in samples and keeps the
  worst, because that is the number an operator wants when a show feels loose. Not a
  `juce::Timer`: spike 05 measured that instrument's own idle floor at 0.76 ms median and
  2.60 ms at the 99th percentile, before doing any work.
- **The `/godot` parameter tree**, published once per tick as an immutable snapshot. It is a
  projection and owns no value: a node under `/godot/cue` reads an attribute of `show.xml`,
  one under `/godot/engine` reads a counter the tick thread keeps, and a write to either is
  the `node.set` command going through the document's single write path. Objects are
  addressed by identity, so a client's subscription survives a reorder. Values the document
  refuses to store because the structure already says them — a cue's index, a container's
  order — are computed here rather than kept in a second place that would eventually
  disagree. Server threads read a published snapshot and never wait on the model, which is
  what keeps a web request out of the way of the GO path.
- **Touch state** (PRD §3.16): while a surface is holding a node it stops being told what
  that node's value is, so a fader under someone's finger is not fought by the engine
  echoing it back. Holding and releasing are commands like everything else, because a replay
  that did not re-apply them would send a different set of messages from the session it
  claims to reproduce.
- **Devices are mounts**: somebody else's namespace, read from an OSCQuery description in
  the bundle and published at its own prefix — or, for a desk that describes nothing, an
  *opaque* device sent to blind. PRD §3.22 makes the template format *be* an OSCQuery
  description, so a capture from a running processor and a file written by hand are the
  same kind of thing to the engine. A captured value is dropped rather than believed, a
  read-only node refuses a write, and every mounted node carries the four declarations
  PRD §3.3 requires.
- **A JSON reader of our own**, because JUCE's accumulates a plain integer literal into an
  `int64` and only switches to the correctly-rounded path when it meets a `.` or an `e`. A
  longer literal overflows in silence: measured, 142 of 19 993 random doubles came back as a
  different number. A namespace file's numbers are somebody else's range bounds, and a bound
  that changes on the way in is one Go.dot would enforce against a target that never declared
  it.
- **The standby pointer** (PRD §3.5): where GO will act, one per cue list. It stores an
  identifier rather than an index or a cue number, so reordering the list moves nothing and
  renumbering during tech moves nothing.
- **The OSC 1.1 codec and a UDP endpoint that says who sent each datagram.** Written
  rather than borrowed, because a decoder reads somebody else's bytes: every read is
  bounds-checked and refuses, an element is parsed against its own extent and not the whole
  packet, the size bound cannot overflow, nesting is capped, and a refusal carries a stable
  atom the log can group by as well as a sentence a person can read.
- **An OSCQuery server on one port**, HTTP and WebSocket, with `LISTEN`/`IGNORE`, binary
  OSC both ways, per-tick coalescing, and pushes withheld from the client that caused them
  and from anyone holding the node. Four HTTP answers to four different questions, because
  a server that collapses them into 200-or-404 makes a client guess. It does not advertise
  itself over mDNS, speaks no TLS (`deps.no-openssl` asserts on the shipped binary that no
  OpenSSL came with it), and resolves an address to exactly one node. Bundle time tags are
  carried and preserved but not scheduled: honouring one would tell a client its timing had
  been respected when it had not.
- **Helper processes for what may crash.** Plugins are scanned, hosted and given their
  editor windows by `wfg` started again as a child, and pictures are drawn by another. A
  plugin host or a renderer that dies is started again, and a crash in one costs at most
  the cues it was serving, never the engine.

### The `wfg` binary

One binary, named in PRD §7. The launchers run `wfg serve` with a window; everything else
is for checking, converting and testing.

| Verb | What it does |
|---|---|
| `serve <bundle>` | Runs a show: the engine, its OSC and OSCQuery ports, and with `--window` the desktop client. `--device[=<name>]` plays through an interface, `--hosted` through no hardware (`--render=<wav>` keeps what came out), `--ui=<dir>` serves the web console. Both ports take 0, bind a free one and print it |
| `devices`, `midi` | The audio devices and MIDI ports this machine has |
| `validate <bundle>` | Checks a bundle against the schema and reports every problem |
| `canon <file>` | Rewrites a show document in canonical form |
| `schema` | Writes the RELAX NG grammar, or checks the committed copy |
| `tree <bundle>` | Prints the parameter tree as OSCQuery JSON, with no server in the way |
| `replay <log>` | Replays a session's log into a fresh engine and checks it reproduces itself |
| `analyse <bundle>` | Works out the spectral colour of every sound a show names, and caches it |
| `plugins` | Scans for VST3, AU and LV2 plugins, out of process, or lists the last scan |
| `import-als`, `import-qlab` | Ableton Live sets, or a QLab 4 or 5 workspace, into a new show |
| `template` | A performance against its show's template cue list: what differs, or bring changes in |
| `associate` | Linux: makes `.wfg` shows open with this copy of Go.dot |
| `commands`, `selftest`, `--version` | The named commands; a headless boot; the linked JUCE and Tracktion versions |

`video-render`, `plugin-host` and `plugin-editor` are the helper processes above. Go.dot
starts them itself; nobody types them.

### Open questions, deliberately unanswered anywhere in this tree

The devplan lists these under Phase 0's *"Needs from the author"*. A default
picked here would be an answer to a question that has not been asked, so there
is no `option()`, no cache variable, no preset value and no `constexpr` for
either of them anywhere in the build:

1. **Default fixed track count.** Spike #4 — graph stability under sustained
   launching, the devplan's first priority — cannot run without one. It takes it
   as `--tracks=N` on the command line.
   *Answered 2026-09-05 by decision G (namespace draft §9): the count is
   `Show/Audio/@tracks`, required in every show and defaulted nowhere — so the build
   still carries none, now by decision rather than by omission.*
2. **Target sample rates and buffer sizes.** Same treatment (`--sample-rate=N
   --buffer=N`). PRD §3.4's "96 kHz / 64 frames" is an arithmetic illustration,
   not a specification.
   *Partly answered: the rate is observed, never set (PRD §6.2, amended in 0.8),
   and a rate that changes under a running show is followed by resampling (the
   author, 2026-09-21). A show file authored at another rate doing the same is
   still (proposed); buffer sizes stay open.*

Three smaller things this scaffold decided and would rather have overruled early
than late: the SPDX suffix is `GPL-3.0-or-later` (`GPL-3.0-only` is equally
defensible, and changing it is one `sed` now and a chore later); the binary is
exercised through `ctest` rather than WFS-DIY's find-the-binary idiom, because
the same executable has to run twice under two locales on three platforms; and
the locale obligation is read as an in-process `setlocale`, which is the only
reading that works identically on all three platforms but is still a reading.

---

## Building

### Prerequisites

**Windows**

1. [Git for Windows](https://git-scm.com/download/win).
2. [Visual Studio 2026 Community](https://visualstudio.microsoft.com/) (free) —
   during install select the **"Desktop development with C++"** workload, which
   brings MSVC, CMake and Ninja. MSVC 19.30 (VS 2022 17.0) is the floor.
3. Python 3 on `PATH`, plus `python -m pip install lxml`. Python runs
   `scripts/check-pins.py` and the two generated-file gates; lxml runs the show
   fixtures through the committed RELAX NG grammar, and the build **refuses to
   configure the test suite without it** — a validator that quietly does not run
   is the same failure as a locale test that quietly reports green. Configure
   with `-DWFG_BUILD_TESTS=OFF` to build the product without the suite.

**macOS**

1. [Xcode](https://apps.apple.com/app/xcode/id497799835) from the App Store, or
   the command line tools: `xcode-select --install`. Xcode 15 is the floor.
2. `brew install cmake ninja ccache`, then `python3 -m pip install lxml`
   (see the Windows note for why it is not optional).
3. **macOS 13.3 is the deployment target**, and that number is not arbitrary: it
   is where Apple's libc++ made `std::to_chars` available for floating-point
   types, which is what Go.dot writes every number with. JUCE's own writer loses
   46% of doubles to a save-and-load round trip (measured; the table is in
   `src/wfg/engine/osc/OscValue.cpp`), and a show file whose numbers change when
   you reopen it is not a show file.

**Linux**

1. GCC 11 or newer (or Clang 14+), CMake 3.22 or newer.
2. `bash scripts/install-linux-deps.sh` — that script *is* the package list, and
   CI runs the same file, so it cannot rot. It brings `python3-lxml` with it;
   see the Windows note for why the build insists on it.

**Optional, on any of the three: Node.js 22.7 or newer** (or 20.19 on the 20 line).
It runs the console's pure modules under `node --test` as the ctest entry
`console.unit`, and nothing else uses it. Without it, or with an older one, the
entry is left out and configure prints a line saying why; the page itself needs
no Node, no build and no install, now or later.

**At run time, none of these is needed to build, and each is optional:** Pure
Data or plugdata, to open a process cue's patch in a program of its own (the
canvas inside Go.dot needs neither); the NDI runtime, for pictures over NDI,
found where its installer put it and never shipped with Go.dot; and FFmpeg,
which Go.dot downloads itself the first time a movie needs converting to HAP.

### Step by step

**1. Clone, then fetch the submodules one level deep**

```bash
git clone https://github.com/pob31/go.dot.git
cd go.dot
git submodule update --init     # every pinned submodule, and none of theirs
./scripts/bootstrap.sh          # scripts\bootstrap.ps1 on Windows
```

That is what CI does: every top-level submodule, then the one nested submodule
that is wanted. `bootstrap` is idempotent — re-run it any time. It populates
that nested submodule, disarms Tracktion Engine's nested SSH JUCE submodule so
the blanket recursive command below cannot bite you later, and checks the pins.

> **Important:** do **not** use a **blanket** `--recursive` on the submodule
> update, and do not use `--depth 1`.
>
> `git submodule update --init --recursive` — with no path after it — descends
> into `tracktion_engine/modules/juce`, whose URL in TE's own `.gitmodules` is
> `git@github.com:juce-framework/JUCE.git` — SSH. Without a registered SSH key it
> fails with `Permission denied (publickey)` three levels down, in a message that
> names neither Go.dot nor Tracktion Engine. Every CI runner is in exactly that
> position. Go.dot pins JUCE itself, so TE's vendored copy is redundant: our
> CMake adds `ThirdParty/tracktion_engine/modules` and never TE's root, and that
> directory can stay empty forever. `git clone --recurse-submodules` is the same
> blanket recursion, which is why step 1 clones first and fetches the submodules
> in a command of its own.
>
> libpd's own nested `pure-data` stays empty for the same reason: Go.dot pins
> Pure Data itself at `ThirdParty/pure-data`, and check (h) of `check-pins.py`
> holds libpd's gitlink equal to that pin.
>
> **There is exactly one exception, and it is scoped by name:**
>
> ```bash
> git submodule update --init --recursive ThirdParty/juce_simpleweb
> ```
>
> `juce_simpleweb` has a nested `asio` of its own that the module cannot compile
> without, and *its* URL is HTTPS (`benkuper/asio`), so recursing into that one
> path is both safe and required. `bootstrap` already does it. The rule is
> therefore not "never recurse" but **"recurse into exactly one path, and name
> it"** — and `check-pins.py` asserts both halves: check (c) that TE's vendored
> JUCE stayed empty, check (f) that `juce_simpleweb/asio` did not.
>
> `--depth 1` fails differently: our JUCE pin is not the tip of `develop`, and a
> shallow fetch reports `fatal: reference is not a tree: 19edd538…`.
>
> `scripts/check-pins.py` enforces all of this, and it is the first CI job.

**2. Configure and build**

```bash
cmake --preset dev              # Ninja Multi-Config -> build/dev/
cmake --build --preset dev-debug
cmake --build --preset dev-release
ctest --test-dir build/dev -C Debug --output-on-failure
```

Note the last line: there is deliberately **no `dev` test preset**. The three
test presets (`ci-linux`, `ci-macos`, `ci-windows`) belong to the matching
configure presets and run in *their* build trees, so `ctest --preset ci-linux`
after `cmake --preset dev` looks for `build/ci-linux/` and reports that there is
no test project there. Locally, point `ctest` at the tree you built.

The suite runs the unit binary twice (once under `C`, once under `fr_FR`); the
product binary once per serialising verb per locale (`canon`, `replay`, `schema`,
`tree`, `validate`, `commands`) plus `--version`, `selftest`, `midi` and the
plugin scans; every replay fixture; the black-box drivers against the shipped
binary, most of them under both locales; and the Python gates — the generated
schema header against the parameter table, every show fixture against the
committed RELAX NG grammar through lxml, the desktop client's boundary
(`client.boundary`), no comment delimiter hidden inside a comment
(`source.comments`), and no OpenSSL in the binary. The GPU
renderer's pictures are held to the reference compositor where the machine has
a device (`video.gpu`). On Windows the native-window checks run too (`ui.*`).
Where Node.js is new enough, `console.unit` runs the console's own tests as well.

On **Windows**, `dev` needs an *x64 Native Tools Command Prompt for VS* (or a
shell where `vcvars64.bat` has run) — Ninja cannot find `cl.exe` from a plain
PowerShell window, and the symptom is `CMAKE_CXX_COMPILER not set`, which looks
like a broken preset and is not. Either open one, or use the `vs` preset:

```powershell
cmake --preset vs
cmake --build --preset vs-debug
```

or simply **File → Open → Folder** on the repo root in Visual Studio, which reads
`CMakePresets.json` and offers `vs` directly.

If `cmake` is not on `PATH` on Windows, the one bundled with Visual Studio is:

```
"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
```

**Raw CMake, without presets** (if you need a build tree somewhere else):

```bash
cmake -S . -B build/manual -G "Ninja Multi-Config" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build/manual --config Debug
```

### Presets

Every preset in `CMakePresets.json` is exercised by CI. That is the rule that
keeps the file from rotting: a preset nobody runs is a preset that stops working
without telling anyone.

| Preset | For |
|---|---|
| `dev` | Everyday local work, Ninja Multi-Config, all platforms |
| `vs` | Windows without a Native Tools prompt; no generator named, so CMake picks the newest VS it finds |
| `ci-linux` / `ci-macos` / `ci-windows` | What each CI job configures |
| `strict` | `WFG_WARNINGS_AS_ERRORS=ON`. Applies to our code only — the vendor sources live in a separate target that never sees the flag. Inherits `dev`, so it runs on all three platforms |
| `strict-ci` | `strict` plus the two `ccache` launchers; what the Linux CI job runs. Keeping them out of `strict` is what lets a Windows or macOS contributor run `strict` without installing ccache |
| `spikes` | `WFG_BUILD_SPIKES=ON`, its own build tree and its own CI job |
| `package` / `package-macos` / `package-windows` | The test build's archive, run by `release.yml` on a tag (or by hand). `WFG_BUILD_TESTS=OFF`; `package-macos` is universal (arm64 + x86_64). See [Test builds](#test-builds) |
| `rtsan` | Clang's real-time sanitizer, the second net under PRD §4.2. **Clang 20 or newer only** — `-fsanitize=realtime` does not exist before it, so this preset fails to configure on MSVC and on Apple Clang, and that is not a defect. Its own CI job installs the toolchain; the suppression list goes in at run time through `RTSAN_OPTIONS`, never in the preset |

Build presets append `-debug` / `-release` (`dev-debug`, `ci-linux-release`, …).

### Targets

| Target | Kind | What it is |
|---|---|---|
| `wfg::deps` | INTERFACE | Include paths, the JUCE/TE macro environment, C++20, platform flags |
| `wfg::warnings` | INTERFACE | The warning policy. Linked to **our** targets only, never to vendor code |
| `wfg::thirdparty` | STATIC | The one place JUCE and Tracktion Engine module sources compile |
| `wfg::engine` | STATIC | The engine library. Its public headers name no JUCE or TE type |
| `wfg::client_model` | STATIC | The desktop client's half with no JUCE type: what a label shows, the theme's tokens, the command each gesture submits — so the tests can assert them with no window |
| `wfg::client_ui` | STATIC | The desktop window itself. Not linked by the engine: `main()` hands it over as a factory, and `client.boundary` holds the split |
| `wfg_pd` | STATIC | Pure Data and libpd, one Pd instance per patch on a thread of its own |
| `wfg_hidapi` | STATIC | hidapi's one platform file, for the SpaceMouse |
| `wfg_syphon` | STATIC | macOS only: Syphon's Metal half and Go.dot's own server on it |
| `wfg` | executable | The product binary (PRD §7's binary name) |
| `wfg_tests` | executable | doctest runner, registered with CTest under both locales |
| `wfg_audio_ui_tests` | executable | Windows only: the native-window checks, against a live device |
| `wfg_windows_launcher` / `wfg_macos_launcher` | executables | `Go.dot.exe`, and the app's launcher on macOS, in a test build |
| `spike01…07_*` | executables | PRD §6.1 validation programs, behind `WFG_BUILD_SPIKES` |

### Test builds

`.github/workflows/release.yml` makes one download per platform for people to
try. Windows and Linux get a folder: the `wfg` binary, `console/` (the web
client), `pd/` (Go.dot's ready-made Pure Data patches), an empty show in
`Untitled/`, example shows in `Examples/`, a launcher (`Go.dot.exe` or
`go.dot.sh`) and a `README.txt` for the tester. macOS gets the same inside `Go.dot.app`, in a
DMG that is **signed with the author's Developer ID, notarized and stapled** by
`scripts/package-macos.sh`, from the secrets in the protected `go-dot`
environment. Windows also gets that folder as an **Inno Setup installer**
(`packaging/windows/go.dot.iss`), which adds the Start menu entry, the
uninstaller and the `.wfg` file type, so a double-clicked show opens in Go.dot;
the workflow installs it, checks it and uninstalls it before uploading. Linux
likewise gets a **.deb** (`scripts/package-linux-deb.sh`): the folder in
`/opt/go.dot`, `/usr/bin/go.dot`, the menu entry, the icons and the `.wfg` type,
installed, checked and removed by the workflow the same way. Windows and Linux
are not signed. `cmake/WfgInstall.cmake` is the list; the extra files
live in `packaging/`.

- **To publish one**, bump `project(VERSION)` in the root `CMakeLists.txt` if
  needed and push a tag whose numbers match it:

  ```bash
  git tag v0.1.0-alpha.1 && git push origin v0.1.0-alpha.1
  ```

  The workflow refuses a tag that disagrees with `project(VERSION)`, builds the
  three archives, smoke-tests each installed binary, and publishes them as a
  GitHub **pre-release** with checksums and the commits since the previous tag.
- **To hand a branch to one tester**, run it by hand (*Actions → Release → Run
  workflow*). The archives are attached to that run for 14 days and published
  nowhere.
- **To make the same folder locally**:

  ```bash
  cmake --preset package && cmake --build --preset package-release
  cmake --install build/package --config Release --component wfg --prefix stage/go.dot
  ```

  On a Mac, `package-macos` in place of `package`, then
  `scripts/package-macos.sh stage/go.dot go.dot.dmg` signs with the Developer ID
  in your keychain and notarizes with the `NOTARY_PROFILE` you name (or ad hoc,
  unnotarized, with neither).

---

## Dependency pins

| Dependency | Version | Commit |
|---|---|---|
| JUCE | 8.0.13+7 (on `develop`) | `37c894f83d379179b2070d437ccd0f1cd9af9576` |
| Tracktion Engine | develop (3.5.0) | `13b51326693e3227ddef91b224114d12af6433ce` |
| juce_simpleweb (the OSCQuery server's HTTP and WebSocket; the author's fork) | `pob31/juce_simpleweb` | `b72ec947548654cdd0a7e72b71ab5826fe574847` |
| spatcore (the author's shared control-plane code, headers only) | main, after PR #18 | `5803830150e408ad76c296cd9edbd5d9eba35dc3` |
| hidapi (the SpaceMouse) | 0.15.0 | `d6b2a974608dec3b76fb1e36c189f22b9cf3650c` |
| sokol (the video renderer's graphics) | master, 2026-10-06 | `401f21f8b7039258c35fef75c11d9a8a0e616771` |
| Spout's SpoutDX (vendored in `ThirdParty/spout`, Windows) | master | `c2bcc12147711d12ace7d5f08e869d774d840f8a` |
| Syphon (`ThirdParty/Syphon`, macOS) | main, 2026-09-21 | `f4761677a45b8034a3c2069ec0f3d2553da81fba` |
| NDI's headers (vendored in `ThirdParty/ndi`; the runtime is the user's install) | 6.3, from DistroAV | `d34b4cc4c590426464bff4d59f88f68829010b4b` |
| Pure Data (`ThirdParty/pure-data`; a process cue's patches) | 0.56-5 | `f009fd8d7b537e209e09898d487fdf1bf547da2b` |
| libpd (`ThirdParty/libpd`; its nested `pure-data` stays empty) | 0.16.1 | `ba0dc63262901d658af8bbda5e619a60fa975e78` |

The load-bearing fact: **Tracktion Engine develop (3.5.0)'s own `modules/juce` gitlink is
byte-for-byte our JUCE pin.** We are not guessing at a compatible JUCE — we are
using the one TE was tested against, while pinning it ourselves so the URL is
HTTPS and the build never enters TE's root `CMakeLists.txt`. Keeping that
equality true is the whole job of `scripts/check-pins.py`.

### Go.dot's patches to Tracktion Engine

Tracktion is built with a short series of changes of Go.dot's own, in
`patches/tracktion_engine/` - today, what a media cue's speed needs (namespace
draft §22.3) and a loop moved while it plays (§33). **The build applies them**: every configure puts the series on the
submodule's working tree before a Tracktion source is read, or finds it already
on and touches nothing (`cmake/WfgTracktionPatches.cmake`). So:

- `git status` shows `ThirdParty/tracktion_engine` as **modified**. That is the
  series, and it is expected. `git -C ThirdParty/tracktion_engine diff` shows it.
- A configure refuses a Tracktion tree carrying anything else, and says how to
  look at it and how to clean it. It never cleans it itself.
- `python3 scripts/te-patches.py status | apply | revert | refresh | new` does by
  hand what the configure does, and what it cannot: `refresh` rewrites a patch
  from the tree after an edit in the submodule; `new` starts one.
- `check-pins.py` check (g) says whether the series is on or fits the pin.

Every file the series touches carries a notice that Go.dot modified it, and
`THIRD_PARTY_NOTICES.md` says the Tracktion Engine in this build is modified.

### Bumping a pin

0. Make sure nothing else is building from this checkout, then take Go.dot's
   patches off Tracktion: `python3 scripts/te-patches.py revert`. Git will not
   check a new commit out over files the patches changed.
1. Move `ThirdParty/tracktion_engine` to the new tag.
2. Read TE's new vendored JUCE SHA:
   `git -C ThirdParty/tracktion_engine ls-tree HEAD modules/juce`.
3. Move `ThirdParty/JUCE` to **that exact SHA**. Not to the tip of `develop`, not
   to the nearest tag.
4. Put the patches back on: `python3 scripts/te-patches.py apply --3way`. If a
   hunk no longer fits, resolve it in the submodule, then
   `python3 scripts/te-patches.py refresh` rewrites the patch from the tree.
5. Run `python3 scripts/check-pins.py`, then configure every build tree and run
   the presets locally on at least one platform.
6. **Commit both gitlinks and the patches in one commit**, so a bisect can never
   land on a skewed pair, or on a pin its patches do not fit.

**Pulling somebody else's pin move** is step 0 and then the pull: revert, pull,
`git submodule update --init ThirdParty/tracktion_engine ThirdParty/JUCE`, and
the next configure applies the moved series.

Never `git submodule update --remote` — it moves a pin to a branch tip behind
your back, which is the one thing a pin exists to prevent. Never a **blanket**
`--recursive`; the only recursion in this repo is the one scoped to
`ThirdParty/juce_simpleweb`, explained above. A deliberate JUCE bump ahead of TE
is possible, but the person doing it says so out loud:
`check-pins.py --allow-skew`.

If JUCE's version number changes, update `WFG_PIN_JUCE` in
`cmake/WfgOptions.cmake` too — `wfg_tests` asserts at **runtime** that
`SystemStats::getJUCEVersion()` contains it, which is what catches a stale
ccache or a stale `.lib` after a bump. There is deliberately no matching runtime
assertion for TE: at the develop (3.5.0) tag, `Engine::getVersion()` still returns
`"Tracktion Engine v3.1.0"`, so only the `"Tracktion Engine"` prefix is asserted.

---

## Naming

The repo is `go.dot`, the documents are `godot-*.md`, the product is **Go.dot**,
and the build system says `wfg` everywhere — targets, binary, CMake identifiers.
The mismatch is deliberate: `wfg` is PRD §7's binary and package name, and a
`GODOT_` prefix would collide with the Godot game engine in every search path a
contributor ever types.

---

## Layout

```
CMakeLists.txt       orchestration only; defines no source target
CMakePresets.json    every preset here is run by CI
cmake/               guards, options, third-party wiring, the install list,
                     and the step that puts Go.dot's patches on Tracktion
docs/                the spec: the PRD, the development plan, the namespace
                     draft, the parameter table, the grammar - and the
                     tester's guides, in English and in French
scripts/             bootstrap, the Linux package list, the pin gate, the
                     generators (schema, shaders, Pd patches, icons), the
                     gates' scripts and the packaging scripts
patches/             Go.dot's own changes to Tracktion Engine, applied by the
                     build (see below)
packaging/           what a test build carries beside the binary: the
                     launchers, the tester's README.txt, an empty show, the
                     example shows, the icons, macos/ (Go.dot.app's launcher,
                     Info.plist, entitlements) and windows/ (the installer)
pd/                  Go.dot's ready-made Pure Data patches, each with a help
                     patch, written by scripts/make-pd-patches.py
clients/console/     the web client the engine serves at /ui; reads by polling,
                     writes binary OSC, no build step, no dependency
clients/desktop/     theme.json, the window's palette, re-read on F5
src/wfg/engine/      wfg_engine, the library
src/wfg/client/      the desktop window, in the same process as the engine
src/wfg/app/         main(), and nothing else
tests/               the doctest suite and every add_test() in the project;
                     tests/blackbox/ holds the drivers and tests/console/ the
                     page's node --test files
tools/               the real-time sanitizer's suppression list
spikes/              throwaway PRD §6.1 validation programs
ThirdParty/          the pinned submodules - JUCE, tracktion_engine,
                     juce_simpleweb (+ its nested asio), spatcore, sokol,
                     Syphon, hidapi, libpd and pure-data - and two vendored
                     folders, spout/ and ndi/
```

Two directories that do **not** exist here, and will not:

- A `clients/tablet/` with its own toolchain. There was going to be one; decision
  V (2026-09-09) says there is not. The tablet client and the console are one
  client — `clients/console/` grows into PRD §3.17's web client, which that section
  requires to be complete on its own because it is the redundancy path. It keeps
  the rule the plan wanted from `clients/tablet/` and keeps it the easy way: no
  toolchain at all, served from disk rather than compiled in, which is what lets
  the page be edited and refreshed while a show is running — the only loop that
  suits work decided by looking at it. A web toolchain inside a CMake tree still
  helps nobody.
- The Phase 11 Rust BLE sidecar lives in the permissively licensed
  [Choufleur](https://github.com/pob31/choufleur_prompt) repo (MIT OR Apache-2.0)
  and is pulled in as a `ThirdParty/` submodule if it is needed at all. There is
  no `src/sidecar/` and no `rust/` under this GPL-3 tree, ever — PRD §3.23's
  licence direction is one-way, and code that starts here cannot go back.

---

## Development

- **Read PRD §4 first.** It is the review criterion for every change.
- Engine before UI, always. Nothing gets a UI before the engine exposes it over
  OSCQuery and a headless test drives it.
- Anything marked *(proposed)* in the PRD is not built without a recorded yes.
  Decision points under *"Needs from the author"* are the author's — surface them as
  questions, do not resolve them by picking the reasonable-looking option.
- Spikes are throwaway. They live in `spikes/`, never migrate into `src/`, and
  each ends in a written pass/fail in `docs/spikes/` — not an exit code, which is
  why CI builds them and does not run them.
- Every serialisation test runs under `fr_FR` as well as `C`. A locale test that
  quietly skips because the locale is missing is exactly the premiere-night bug
  the rule exists to prevent, so the test runner exits non-zero rather than
  reporting green.
- Every source file carries the GPL header and `SPDX-License-Identifier:
  GPL-3.0-or-later`.

## Contributing

1. One concern per PR.
2. Make sure it compiles on your platform, and run `ctest` — both locales.
3. Run `cmake --preset strict && cmake --build --preset strict-debug` before
   opening the PR; CI runs the same thing (as `strict-ci`, which is `strict` plus
   ccache) and `-Werror` applies to your code. `strict` inherits `dev`, so on
   **Windows** it wants the same *x64 Native Tools Command Prompt* that `dev`
   does; from a plain PowerShell window, configure the `vs` preset with the flag
   instead:

   ```powershell
   cmake --preset vs -DWFG_WARNINGS_AS_ERRORS=ON
   cmake --build --preset vs-debug
   ```
4. Update `docs/` when the behaviour it describes changes.
5. A change to Tracktion Engine goes in `patches/tracktion_engine/`, never as a
   commit in the submodule: edit the submodule's files, then
   `python3 scripts/te-patches.py refresh` (or `new` for a patch of its own),
   and carry the modification notice the other patches carry.

<!-- No CI badge: the repo is private until alpha, and a badge for a private
     repo renders as a broken image for everyone outside it. Add one at the
     public alpha. -->

---

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0).

Copyright (c) 2026 Pierre-Olivier Boulant

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <https://www.gnu.org/licenses/>.

### GPL v3 Key Principles

- **Freedom to use**: You can run the software for any purpose
- **Freedom to study**: You can examine and modify the source code
- **Freedom to distribute**: You can share copies of the software
- **Freedom to distribute modifications**: You can share your modified versions

**Important**: Any derivative works must also be licensed under GPL v3, ensuring
the software remains free and open source.

### A note on JUCE

Go.dot is licensed **GPL-3.0**, matching WFS-DIY. JUCE 8 and 9's open-source
path is **AGPLv3**, not GPLv3; Tracktion Engine's is GPLv3. Stated here once as
a fact, and not argued anywhere else in this tree. See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for the per-dependency
licence facts.
