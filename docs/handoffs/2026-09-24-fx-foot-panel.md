# Handoff — the FX panel at the foot of the desktop window (Phase 9a, the last piece of 9a.9)

*Written 2026-09-23, late, by the session that built Phase 9a.0–9a.10. For the session that
builds this component. Everything below is on `main` at 48adf50 or later.*

## What this is, in one paragraph

A media cue's inserts, drawn at the foot of the desktop window the way the EQ is: one row per
entry of the show's plugin set, in chain order, each with the entry's name, its state in a word,
a switch that puts the entry in this cue's signal, and the entry's parameters as sliders with the
plugin's own text beside them. A drag on a slider writes the cue — one `node.set` per change, on
one address per parameter — so the engine's coalescing makes a turn one undo step, and a voice
playing that cue follows within two ticks. Nothing in it is worked out by the client: every value,
name, word and sentence is read off the tree.

## What already exists (do not rebuild)

| piece | where | what it gives you |
|---|---|---|
| the reading | `src/wfg/client/model/Fx.{h,cpp}` — `model::readFx (snapshot, cueId)` | `FxReading { present, strips, notice }`; one `FxStrip` per set entry with `pluginId`, `name`, `index`, `state`, `problem`, `fxId` (empty until the first switch-in), `enabled`, and `params` — each `FxParameter` with `index`, `name`, `shortName`, `unit`, `value` (0..1), `defaultValue`, `text`, `discrete`, `steps`, `bipolar`. `strip.present()` says whether the cue has an Fx for it. |
| the addresses | same file | `model::fxAddress (fxId, "enabled")`, `model::fxParameterAddress (fxId, n)` → `/godot/fx/<id>/p<n>` |
| the gesture | `src/wfg/client/model/Gestures.{h,cpp}` | `gesture::createFx (cueId, pluginId)` → `fx.create`; pinned in `tests/ClientTests.cpp` |
| the tests of the reading | `tests/ClientTests.cpp`, case "client: a cue's inserts are one strip per entry of the set…" | the shape a strip has before and after `fx.create`, the notices for a memo and for a show with no set |
| the shape to copy | `src/wfg/client/ui/EqPanelComponent.{h,cpp}` | `Actions { set, reset, say }`, `show (const model::FootReading&)`, editable number boxes, toggles, the write-on-each-change idiom (`actions.set (address, text)`; no throttle of the panel's own — the engine coalesces) |
| the Plugins tab | `src/wfg/client/ui/ShowSettingsWindow.cpp`, `PluginsPage` | how a state word and its sentence are drawn, `Look::colour (theme, "ink" / "ink-dim" / "panel" / "panel-in" / "panel-raised")`, `Look::font (theme, 14.0f)` |

## What is missing, file by file

1. **`src/wfg/client/ui/FxPanelComponent.{h,cpp}`** — the component. Add both to
   `src/CMakeLists.txt` after `wfg/client/ui/EqPanelComponent.cpp`.
2. **`src/wfg/client/model/Foot.h`** — `Subject::Kind` gains `fx` (after `eq`); `FootReading`
   gains `FxReading fx;` (include `<wfg/client/model/Fx.h>` beside `Eq.h`).
3. **`src/wfg/client/model/Foot.cpp`** — `followsPick (Kind::fx)` returns true; `readFoot` gains
   the branch the EQ has: `if (subject.kind == Subject::Kind::fx) { out.fx = readFx (snapshot,
   subject.objectId); if (! out.fx.present) out.notice = out.fx.notice; }`.
4. **`src/wfg/client/ui/FootPanelComponent.{h,cpp}`** — `Actions` gains
   `std::function<void (const std::string& cueId, const std::string& pluginId)> createFx;`; a
   member `std::unique_ptr<FxPanelComponent> fx;`; the `build()` switch gains `case Kind::fx`
   in the EQ's shape (hand `actions.set`, `actions.createFx` and a `say` that sets `note`);
   `show` calls `fx->show (reading)`; `resized` gives it the area; the title word is `"FX"`.
5. **`src/wfg/client/ui/Client.cpp`** — `inspectorActions.openPanel`: `else if (subject == "fx")
   wanted = model::Subject::Kind::fx;`; `footActions.createFx = [this] (cue, plugin) { send
   (gesture::createFx (cue, plugin)); };` beside `footActions.createSend`.
6. **`src/wfg/client/model/Inspector.cpp`** — `offer ("FX, the show's inserts on this cue", "fx");`
   after the EQ's offer (line ~531). Then in `tests/ClientTests.cpp` the media openers become
   THREE (the case at line ~2193 says `REQUIRE (onMedia.size() == 2)` and checks `[1] == "eq"`;
   add `[2] == "fx"`); the "openers are a run at the back" walk further down already allows a run.
7. **`tests/RunPaneUiTests.cpp`** — a case in the EQ panel case's shape (line ~780): the numbers
   are drawn, the switch on a strip with no Fx calls `createFx` once, the switch on a strip with
   an Fx writes `enabled`, a slider writes one `p<n>` with a canonical number, a discrete
   parameter offers its steps. And a picture case gated on `WFG_SNAPSHOT_DIR` (line ~904), which
   is how the author looks at it with no screen.
8. **`docs/godot-namespace-draft-0.1.md` §17.8 and §17.12** — say the panel is built; the
   §17.12 sentence that says it is not is the one to replace.

## The behaviour, decided

- **A row per set entry, in chain order**, present or not — a panel that drew only the cue's Fx
  children would be a panel you cannot switch a second entry in on (Sends.h's argument).
- **The switch.** On a strip with no Fx (`! strip.present()`): send `createFx (cueId,
  strip.pluginId)` and nothing else — `enabled` defaults to true, so creating is switching in.
  On a strip with an Fx: `actions.set (fxAddress (strip.fxId, "enabled"), "true" / "false")`.
- **The state word and its sentence are drawn in words** (`loaded`, `loading`, `missing`,
  `failed`, then `strip.problem`); colour is never the sole carrier (PRD §4.8). A `failed` or
  `missing` entry keeps its switch and sliders — the cue's values are the cue's whether the
  machine has the plugin tonight or not; the strip says the voice plays dry.
- **A slider per parameter**, 0..1 as the plugin takes it, labelled with `name` (or `shortName`
  when narrow) and the `unit`; `bipolar` ones drawn from the centre; `discrete` ones as a
  chooser over `steps` writing `index / (steps − 1)`. The value text beside it is `t<n>`
  (`param.text`), empty until the cue has an Fx.
- **A drag writes on every change**: `actions.set (fxParameterAddress (strip.fxId, n),
  <canonical number>)`. The engine's coalescing (same address, same origin, within 25 ticks)
  makes the drag one undo step; two parameters are two steps; that is tested in
  `tests/FxRowsTests.cpp` and nothing in the panel needs to help it.
- **A strip whose parameters are unknown** (no catalogue on this machine: `params` empty) draws
  its name, word, switch, and the sentence "no catalogue on this machine — run `wfg plugins
  --catalogue=<identifier>`"; the identifier is on `/godot/plugin/<id>/identifier`.
- **Not in the panel**: restart (the Plugins tab has it), preset (the tab), adding entries (the
  tab). A panel that did those would be a second door to the set.

## Engine facts the panel leans on

- `p<n>` and `t<n>` exist only once the machine's catalogue knows the plugin — from the cache,
  from the child's report, or from `wfg plugins --catalogue`. The test gain's is built in, so
  the fx fixture and the harness always have `p0`/`t0`/`p1`/`t1`.
- The tree's document half follows the plugin table's and the catalogue store's revisions, so a
  child coming up shows in the next publish with nothing on the client's side to do.
- `fx.create` refuses a second Fx for an entry the cue already has (`bad-value`); the panel
  should never send it for a present strip.
- Values in the cue's row are doubles spelled canonically; the panel reads `param.value` and
  writes with `osc::formatDouble` (as `EqPanelComponent::writeNumber` does).

## How to see it

```
wfg plugins --scan                       # once; the fx fixture uses the built-in test gain anyway
python tests/blackbox/phase9a_fx.py      # the path end to end, no window
wfg serve <a copy of tests/fixtures/bundles/fx with media/tone.wav> --sample-rate=48000 --buffer=64 --hosted --window
```

In the window: Show settings → Plugins shows "Test gain … loaded"; pick the media cue; the
inspector offers "FX, the show's inserts on this cue"; switch it in; drag Gain and watch `t0`
say the decibels; Undo once takes the whole drag back. With `WFG_REAL_VST3` scanned and added
to the set from the tab, the same with a real plugin's fifteen parameters.

## Verify before pushing

`cmake --build --preset vs-debug --target wfg_tests --target wfg --target wfg_audio_ui_tests`;
`wfg_tests --test-case="client*"`; `ctest -R "ui\.|client.boundary"`; `python
scripts/check-client-boundary.py` (the model half names no JUCE type; no `childrenOf` in the
client; one call site per door); the page tests `node --test tests/console/*.test.mjs`.

## Traps this session met

- **The classifier.** The first attempt to write this component's header was cut off by the
  session's safety layer mid-file, for no reason in the content anyone could name; the truncated
  file was deleted. Write the two files plainly and in one go, in a fresh session.
- `isYes()` takes a `Flag`, not a string: `isYes (flag (snapshot, address))`.
- `Access::readWrite` is the enumerator for a rw node; `Kind::state` for a value.
- The gesture-pin test builds its own registry; a new gesture whose command is not registered
  there fails with "no such command" — `fx.create` is a document command and is already there.
- Several working-tree files are CRLF; scripts that edit them keep each file's own endings. The
  shell mangles quoted heredocs with apostrophes: write Python patches to a file and run them.
- `tests/blackbox/common.find_binary()` prefers a stale `build/ci-windows/` binary; set
  `WFG_BINARY` when driving by hand.
