# Handoff: what is owed after sidechain keys and the Dynamics stage (§141)

*Written 2026-11-22 by the go.dot session that built namespace draft §141 (stages SC.0-SC.9, closed at
`5d065bab`), for the next Claude session in go.dot and for the author. Everything named below was read at that
commit.*

## In one paragraph

§141 is closed. An effect can now listen to a **key**: a plugin with a sidechain input on any cue, mic channel or aux,
and the **Dynamics** stage that every one of those now has after its EQ (a compressor, a gate, a filter on what they
hear, peak or RMS). A key is picked from what the show already has: a named input, an aux, or "this cue, after its
level". The author asked the next session to close **five features §141 left owed**:

1. fades that move the Dynamics stage;
2. the stage on the D700's and the Stream Deck+'s pages;
3. Live's sidechain routing at the Ableton import;
4. an output as a key;
5. the stage on the Android remote.

Read §141 first (`docs/godot-namespace-draft-0.1.md`, `## 141.`, near the end): the decisions with their letters
(BHL-BHW), the built notes per stage, and the owed list this handoff expands.

| # | Feature | Size | Needs the author first? |
|---|---|---|---|
| 1 | Fades move the stage (`fade/dyn`) | Medium: the engine half exists | No: it follows the EQ's pattern line for line |
| 2 | D700 and Stream Deck+ pages | Medium | **Yes**: which button and which key turn the page |
| 3 | Live's sidechain at the import | Medium-large | **Yes**: Live keys from a *track*, Go.dot only from an aux or an input |
| 4 | An output as a key | Medium | **Yes**: one block late at best; is that acceptable? |
| 5 | The Android remote | Medium, another repo | Small: where the controls go on the tablet |

Ask the author's questions together, in one round, before building 2-4. AskUserQuestion's option labels are yours:
**never quote them back as the author's words** (§141 records a slip of exactly that).

## How this house works (read before the first commit)

- **Push straight to main**, commit by path, one stage per commit, message ending with the attribution line.
  `CLAUDE.md` is PRD §4 verbatim and is the review criterion for every change.
- **Tests in both locales** for each stage (`--wfg-locale=fr-FR`; `std::stod` is a trap in French: parse with
  `osc::parseDouble`), then the **whole suite file by file** with a timeout per file (one long process can hang on
  the audio device). The scratchpad pattern: loop `tests/*Tests.cpp`, `--source-file="*<name>"`, `timeout 1500`.
- **Parameter rows:** `docs/parameters/godot-parameters.csv` -> `python scripts/generate-schema.py` -> build ->
  `wfg schema --out=docs/schema/show.rng`. `-Wswitch-enum` is an error on the strict build: a new enum value needs a
  case in every switch.
- **This PC is shared.** Several Claude sessions build here (WFS-DIY, Likyu, OSCQuery). Before a Release build or a
  whole-suite run: `ListAgents`, and if a peer is busy send the job to the NUC (`ssh pob@172.22.1.29`, its own
  worktree under `~/farm-test/<task>/`; `~/go.dot-farm` is parked on another session's trial, leave it). The WFS-DIY
  session pings before its big builds and caps them at four compilers; do the same. Strict GCC 15 and rtsan
  (Clang 21) on the NUC before pushing audio-thread code: the script in `~/farm-test/sidechain/run.sh` is a template.
- **Words on screen are the author's.** Offer them; record in §141 which are his and which are yours.

## 1. Fades that move the Dynamics stage

**What exists.** A fade already moves a target's EQ numbers: the fade's row `fade/eq` is a list `row:value ...`,
read by `cue::readFadeMoves` (`src/wfg/engine/cue/FadeMoves.cpp`, `isMovableEqRow`, `eqRowRange`, `eqRowDomain`),
landing in the run's `moved` map as `eq/<row>`. **`Runner::dynOf` already reads `moved` under `dyn/<row>`**
(`Runner.cpp`, beside `eqOf`), so the engine's last step is in place.

**To build.**
- A row `fade,dyn` like `fade,eq` (the numbers only: `dynThreshold`, `dynRatio`, `dynAttack`, `dynRelease`,
  `dynKnee`, `dynMakeup`, the gate's five numbers, `dynKeyLowCut`, `dynKeyHighCut`; never a switch, the detector or
  the key - a fade moves no switch, §26).
- `readFadeMoves` takes the list: `isMovableDynRow`, ranges from the CSV, a domain (decibels in dB, times and
  frequencies logarithmic, as the EQ's).
- `FadeMoveRows.cpp` (the fade's own rows), the validity test at `FadeMoves.cpp` ~130 (`kind == "eq"`).
- Doh! restores what a GO's fade moved (§94). Check that the dyn moves are restored with the EQ's.
- Client: `model/FadeMix.cpp` reads the fade's `eq` list (~340); add the dyn rows to the fade's mixer; a
  `readFadeDyn` like `readFadeEq` (`model/Eq.cpp`); `panelFits` in `model/Foot.cpp` gives `Subject::Kind::dyn` to a
  fade on a sound, as the EQ's (`fadeOnSound`); the Dynamics panel shows a fade's target with the fade's values laid
  over, as the EQ panel does (`EqReading::fadeId`, `fadeMoves`).
- Tests: `FadeMoves` cases beside the EQ's, a GoTests or SamplerTests fade moving `dynThreshold` under a sounding
  cue, a client reading, a picture.

## 2. The D700's and the Stream Deck+'s pages

**What exists.** Pages are `surface::Page { show, eq, send, fx, loop, pan, width }` (`SurfacePages.h`), published as
`surface/page` (CSV range `show|eq|send|fx|loop|pan|width`). The D700's rotaries: `SurfaceBridge.cpp` (the EQ page
~1264-1290, its button ~1827, its LED ~3214, its writes ~3703 and ~3927). The Stream Deck+'s dials:
`DeckBridge.cpp` (`DeckRole::pageEq` ~772 and ~1648, page counts ~936, `eqWrite` ~1463). The foot follows a page
(`model::footForSurface` in `Foot.cpp`: `"eq"` -> the EQ panel); add `"dyn"` -> `Subject::Kind::dyn`.
Decisions: `rotary-pages`, `surface-pages-design`, `streamdeck` in memory and namespace §17.16-17.18, §95, §101-102.

**The author's questions.**
- *D700:* which press puts the Dynamics page up? Its buttons are fixed. Candidates: a second press on EQ alternates
  EQ and Dynamics; or a held FX. How many rotaries per page? The stage has thirteen numbers. A proposal: page 1 the
  compressor's six plus the switch, page 2 the gate's five plus the key filter's two.
- *Stream Deck+:* its four top keys pick Send, EQ, FX and Loop today, so Dynamics needs a fifth place (a page of keys,
  or sharing one). Four dials per page: threshold, ratio, attack, release, then make-up, knee, gate threshold, gate
  range...

**To build.** `Page::dyn` (word `dyn`, CSV range), a write per control like `eqWrite` onto the cue's `dyn*` rows
through the same paths (the lock rides live: `LiveEdits`), the strip's display words, and the page count. Tests:
`SurfaceBridgeTests` and `DeckBridgeTests` beside the EQ page's; the foot follows the page (`ClientTests`).

## 3. Live's sidechain routing at the Ableton import

**What exists.** `src/wfg/engine/import/AlsReader.cpp`, `AlsWalk.cpp`, `AlsImport.cpp` (§29). The walk already reads
a track's devices: EQ Eights onto the cue's EQ, third-party plugins into the chain, Live's own devices named in the
notes (`liveDevices`). Return tracks become auxes (§93, `AlsWalk.cpp` ~200-340). The author's real sets that were
probed are listed in memory `sturbzep-als-findings` and §121.

**What Live writes.** Its Compressor (`Compressor2`), Glue Compressor and Gate carry a `<SideChain>` with an on/off
and a routed input whose target names a track's output (`AudioIn/Track.<n>/...`). Read a real set before writing
the parser: the XML differs between Live versions.

**The author's question.** Go.dot keys only from an aux or an input (BHL). A Live sidechain from *track X* would
become, for example, a silent aux made for it (no outputs) with a send from X's cues at unity, and the ducked cue
keyed by that aux: one block late (BHP). Or the importer only says, in the import's report, that the sidechain was not
carried. And Live's Compressor maps onto the Dynamics stage (threshold, ratio, attack, release, knee, make-up, its
Peak/RMS model) - convert it, or leave it as a named device?

**Tests.** `AlsWalkTests` and `AlsImportTests` with a small hand-made `.als` fixture, both answers' report lines.

## 4. An output as a key

**Why it is different.** Outputs are summed after the graph: each track's `CueOutputPlugin` writes into the wide
device buffer. An output can only be read as a key **from the block before**, at best one block late - and always
late, even for a cue rendered after the output was "finished". Say so in the words and in §141.

**Sketch.**
- Capture the output buses' channels at the end of each block (where the `BlockSink` is fed in `AudioHost`), into a
  per-bus stereo row - a third `AuxBus`-shaped store, folded to two as BHT.
- `KeySource` needs a fifth kind. Today `Kind` is two bits (none, input, aux, self: full) with width six bits and
  first twenty-four (`audio/KeySource.h`, `packed`/`unpacked`). Repack: kind three bits, width six, first
  twenty-three. Update `KeySource` tests and the region-v7 tests that pack keys.
- `KeyReader::readOutput`, `Runner::keySourceOf` resolving a `Bus` id, `ShowDocument`'s `Keys` validator (an output
  bus becomes a legal key), the client menus (`model::keySources` in `Fx.cpp`: add the show's outputs, "<name> ·
  output"), the `no key` problem's test of what is there.
- **The author's question:** worth it at one block late? His original question named "a cue or output". A ducker
  keyed by the main PA mix is the use; feedback loops (a cue ducked by an output it feeds) are possible and harmless
  but should be said.

## 5. The Android remote

**What exists.** `D:\dev\go.dot-remote` (github `pob31/go.dot-remote`, app 0.1.9, Kotlin, Compose). It edits the EQ
through `model/Editable.kt` and `ui/widgets/EqKnobs.kt` (laws in `EqLawsTest`); the Surface tab has EQ and Sends
pages. It reads the tree over the WebSocket stream. Memory `android-remote-decisions` has the repo's rules, the
tablet (adb serial, path), and how a release is cut (GitHub Releases, the version code).

**To build.** The Dynamics rows on the Cues page beside the 16 EQ controls (the switch, peak/RMS, the compressor, the
gate, the key filter, the key as a list), a Dynamics page on the Surface tab if the author wants one, en and fr words,
`EditableTest` cases, and a release when the author asks for one (0.1.10, code 12).

## Likyū, and the test that is still owed

Likyū (`D:\dev\likyu`, v0.1.0, the author's own dynamic EQ: eight bands, each with its own detector **keyed from its
input or from a sidechain**) is being optimised right now by other Claude sessions (`likyu-*` in `ListAgents`).
Two consequences:

- **It is the real plugin the §141 bench still owes.** The sidechain path has only met Go.dot's own test plugins
  (`godot:test-duck`, the four `PickyProcessor` cases). Loading Likyū's VST3 (and its AU on the Mac farm) as a keyed
  insert checks the child's probe (`PluginLoad::probeKey`), the layout switch, and the key reaching a vendor's
  sidechain bus: `WFG_REAL_VST3=<path> --no-skip` on the proxy cases, then by ear.
- **Do not touch Likyū's repository or its builds.** Use its released 0.1.0 installer (or ask the Likyū session for a
  build path). If a fault is found in how Likyū lays out its sidechain, write it up for the Likyū session through
  SendMessage, not as a patch. Likyū's parameter IDs are not frozen: a Go.dot show that saves its state may not
  reopen against a later build.
- **Done since, on Windows VST3, by the Likyū session (namespace draft §142, 2026-11-22):** Likyū through the child
  as a real plugin, the keyed case included - `proxy: a real VST3's sidechain, keyed through the child, turns it
  down when the key is loud` (`WFG_REAL_VST3`, `WFG_REAL_KEYED_STATE`, `WFG_ENGINE_FOLDER`; how to make the state and
  scan Likyū alone is in §142 and in Likyū's `docs/bench/2026-11-22-godot.md`). The probe found a stereo sidechain,
  the child laid it out keyed and gave it back unkeyed, and the key cut by exactly the band's range. **Still owed
  here:** the same on the Mac farm with Likyū's AU, and by ear. And §142's BHZ is the author's: a VST3 wider than
  eight channels is laid out stereo by the ladder, so a wider cue passes it dry.

## Also owed, the author's (not this session's)

By ear on a Release build: a console mix on an input ducking cues through a plugin; a silent aux fed by a track keying
another cue; a cue's own compressor easing as its DCA comes down; the keyed gate; peak against RMS.

## Suggested order

1 (no question, engine half exists), then the round of questions, then 5 and 2 (both reuse the panel's rows), then 4,
then 3 (the largest, and the one most in need of a real set). Close each in §141's "Built, as it goes" list, update
the tester guides in both languages and the README, and keep memory's `sidechain-keys-decisions` current.
