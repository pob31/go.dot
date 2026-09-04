# Go.dot — Reuse map

**Draft 0.1**, 2026-09-04. What the author's other projects already provide, per Go.dot
phase, and what stops each piece being used as-is. Written so the next phases start from
it instead of rediscovering it. Paths are relative to the sibling checkouts on the
development machine (`D:\dev\WFS_DIY_v1`, its `spatcore` submodule, `D:\dev\juce_simpleweb`)
and to the repositories `github.com/pob31/{WFS-DIY,spatcore,juce_simpleweb}`.

The PRD is deliberately not amended by this file: `CLAUDE.md` is PRD §4 byte for byte and
CI enforces it, so amendments go into the PRD by the author, citing this map.

---

## The three sources

| Source | What it is | Licence | State |
|---|---|---|---|
| **WFS-DIY** | the shipping wave-field-synthesis app; JUCE 9.0.1, Projucer-built | GPL-3 | production |
| **spatcore** (`WFS_DIY_v1/spatcore`, pinned at `7e7ed63`) | the shared real-time core extracted from WFS-DIY: `rt/`, `dsp/`, `wfs/`, `reverb/`, `gpu/`, `control/{osc,state,mcp}`, `controllers/`, `ui/`, `io/` | **no LICENSE file** ("core code follows the consumer projects' licensing") | consumed by WFS-DIY at source level; CMake targets exist for XOA / Tight-WFS |
| **juce_simpleweb** (`pob31/juce_simpleweb`, fork of `benkuper/juce_simpleweb`) | JUCE module: HTTP + WebSocket server on one port, WebSocket client; Simple-Web-Server (MIT) over standalone asio (BSL-1.0); TLS optional | GPL-3 | **the copy vendored inside WFS-DIY is ahead of the fork** (see below) |

XOA and Tight-WFS were looked at only for conventions; both are under construction and
nothing here depends on them.

## Two constraints that decide what can be lifted verbatim

1. **JUCE 8.0.13 versus JUCE 9.0.1.** Go.dot is pinned to JUCE 8.0.13 because that is the
   SHA Tracktion Engine 3.5.0 was tested against (`scripts/check-pins.py`); WFS-DIY and
   spatcore are on JUCE 9.0.1. Anything in spatcore that uses a JUCE 9 API does not compile
   here. Known instance: `control/osc/OSCParser.h` constructs `juce::OSCArgument(true)`,
   and JUCE 8's `juce_OSCArgument.h` knows only int32/float32/string/blob/colour.
   JUCE-free headers (`rt/RtThreadPriority.h`) and `juce_core`-only headers
   (`rt/RtSnapshot.h`) are safe.
2. **One JUCE compile.** `cmake/WfgThirdParty.cmake` compiles every JUCE and Tracktion
   module exactly once, in `wfg_thirdparty`. spatcore's CMake targets link `juce::*`
   module targets, which would compile the module sources a second time into
   `spatcore-*` — so spatcore is consumed at **source level** (as WFS-DIY itself does),
   never through `add_subdirectory(spatcore)`.

## Per phase

### Phase 1 — document and tree (this phase)

| Need | Reuse | Status |
|---|---|---|
| HTTP + WebSocket transport for OSCQuery | `juce_simpleweb` (`SimpleWebSocketServer`, `SimpleWebSocketClient`) | pinned as a submodule once the fork carries WFS-DIY's patches |
| OSCQuery server shape | `WFS_DIY_v1/Source/Network/OSCQueryServer.{h,cpp}`: HOST_INFO, full tree, attribute queries, LISTEN/IGNORE, 30 ms coalesced binary pushes, PATH_CHANGED, per-IP echo suppression | **pattern only** — the transport shell is generic, the namespace builders are WFS-specific, and its HTTP handler walks the live ValueTree from a worker thread (a race Go.dot's snapshot removes). Go.dot's server is written in-tree behind a namespace-provider seam |
| OSC codec | `spatcore/control/osc/{OSCParser,OSCSerializer}.h` | **not usable**: JUCE 9 only, drops bundle time tags, cannot serialise `T`/`F`, unknown tags throw. Go.dot writes its own full OSC 1.1 codec, shaped so it can be lifted into spatcore |
| UDP/TCP receivers with sender IP | `spatcore/control/osc/{OSCReceiverWithSenderIP,OSCTCPReceiver}` (raw-data callback path) | **shape reused** (raw datagram + sender IP:port), code not: their legacy path pulls `OSCParser.h` in |
| Origin tagging | `spatcore/control/osc/OscTransportTypes.h` (`OriginTag`, thread-local `OriginTagScope`) | **concept only**: a thread-local cannot survive Go.dot's queue hop; the origin travels on the event |
| Tick-thread priority | `spatcore/rt/RtThreadPriority.h` (MMCSS "Pro Audio" via runtime-loaded avrt, mach time-constraint, SCHED_FIFO; JUCE-free) | **used as-is** |
| Black-box test convention | `WFS_DIY_v1/tools/validation/control-replay/` (`common.py`, `osc_replay.py`, `oscquery_echo_check.py`, `session_roundtrip.py`): stdlib Python, goldens, exit codes 0/1/2/3, launch the app, write over OSC, read back over OSCQuery | **adopted**; the RFC 6455 client in `oscquery_echo_check.py` seeds `tests/blackbox/` |
| XML persistence | `spatcore/control/state/XmlPersistence` | **not usable**: writes a `<!-- Created: -->` timestamp header (which is why WFS-DIY's harness has to normalise) and uses JUCE's default XML formatting; Go.dot's document must be canonical and byte-identical |

### Phase 2 — first sound

| Need | Reuse |
|---|---|
| Message → real-time hand-off | `spatcore/rt/RtSnapshot.h` — POD snapshot under a `SpinLock`, the pattern from the 2026-07 binaural RT-safety fix |
| Apple Silicon audio workgroups | `spatcore/rt/AudioWorkgroupCoordinator.h`; note `docs/spikes/README.md` leaves the workgroup question open and Tracktion's own switch is `EditPlaybackContext::enableAudioWorkgroup(true)` |
| Device layer | `spatcore/io/DeviceHost.h` (explicit channel masks that stick), `io/DeviceIoCallback.h` (hardware-indexed buffer, no channel cap), `io/TestSignalGenerator.h` (500 ms protective ramp) — the RME Digiface Dante rig of PRD §6.2 |
| Lock-free rings | `spatcore/rt/LockFreeRingBuffer.h` (SPSC float ring), `rt/SharedInputRingBuffer.h` |
| The observed sample rate replaces `--sample-rate` | — |

### Phase 2/4 — closed-loop cues and mock targets

| Need | Reuse |
|---|---|
| An OSCQuery *client* | `WFS_DIY_v1/Plugin/Source/Shared/OscQueryClient.{h,cpp}` |
| OSC over TCP | `spatcore/control/osc/OSCTCPReceiver` — 4-byte big-endian length prefix, ≤16 clients — the framing WFS-DIY already speaks, so `verified` writes can go over TCP |
| Mock target and golden drivers | `tools/validation/control-replay/` again |

### Phase 5 — desktop UI and undo

| Need | Reuse |
|---|---|
| Per-domain undo | `spatcore/control/state/TreeParameterStore`: `juce::UndoManager` per app-declared domain, `ScopedUndoDomain`, `ScopedUndoSuppression` (cue-driven recalls must not bury the operator's edits), write interceptor, post-write hook. This is the "per-domain undo histories from WFS-DIY port directly" of PRD §3.20 — the **mechanisms** port; the `(paramId, channelIndex)` API is WFS-shaped |
| Two things to design before porting | undo must itself be a logged command to stay replayable; `UndoManager::ActionSet` reads `Time::getCurrentTime()` (`juce_UndoManager.cpp:73`) and needs an exception in the "no clock reads while applying" rule |
| Shared widgets | `spatcore/ui/` (EQ display, band toggle, patch matrix) — palette and strings injected by the app |

### Phase 6 — control surfaces; Phase 11 — Stream Deck, SpaceMouse

| Need | Reuse |
|---|---|
| Rate endpoints | `spatcore/controllers/spacemouse/SpaceMouseDevice.h` (HID) — the `rate` class of PRD §3.16 |
| Bitmap renderables | `spatcore/controllers/streamdeck/{StreamDeckDevice,StreamDeckManager,StreamDeckPage,StreamDeckRenderer}.h` |
| Touch, Lightpad | `spatcore/controllers/touch/` (evdev on Linux), `controllers/lightpad/` (ROLI BLOCKS) |
| Mapping vocabulary | `spatcore/controllers/{ControllerDevice,ControllerEvent,ControllerMapping}.h` — check §3.16's absolute / relative / rate classes against these before implementing |
| Build glue | hidapi wiring in `spatcore/cmake/SpatcoreConsumer.cmake` (`spatcore_add_hidapi`) |

### Phase 7 — tablet

The web client is Go.dot's own (PRD §3.17). WFS-DIY's Android remote (`WFS_control_2`)
is the "hand-maintained parameter table on both sides" the PRD contrasts it with.

### MCP — a client the PRD names but no phase schedules

`spatcore/control/mcp/` is a complete Streamable-HTTP MCP server on the same
juce_simpleweb transport: JSON-RPC transport, dispatcher with message-thread marshalling,
tool registry, prompt and resource registries, three-tier confirmation, AI undo log. PRD
§3.2 lists MCP among the clients of the OSCQuery surface; the PRD should say whether
Go.dot embeds this server or bridges MCP → OSCQuery externally. The port block reserves
7410 either way (WFS-DIY 7400).

## Gaps worth a spatcore decision (the author's, not Go.dot's)

- `control/osc/OSCParser.h` / `OSCSerializer.h`: JUCE 9 only; bundle time tags parsed and
  discarded, always written as "immediately"; `T`/`F` arguments serialise to nothing (a
  desync). Go.dot's codec could be lifted into spatcore.
- The vendored `WFS_DIY_v1/ThirdParty/juce_simpleweb` differs from `pob31/juce_simpleweb`
  at: `juce_simpleweb.h` (`#ifndef SIMPLEWEB_SECURE_SUPPORTED` guard, `_WIN32_WINNT 0x0A00`,
  `NOGDI` removed), `SimpleWebSocketServer.{h,cpp}` (`#if SIMPLEWEB_SECURE_SUPPORTED` around
  the HTTPS `serveFile`). `spatcore/cmake/SpatcoreConsumer.cmake` relies on the guard; the
  fork cannot build TLS-off without it. Push the patches; upstream to benkuper.
- `spatcore` has no LICENSE file.
- `SpatcoreConsumer.cmake` strips `libssl libcrypto z` from juce_simpleweb's interface
  link libraries — the macOS/Windows spellings. The module also declares `linuxLibs:
  ssl,crypto`, which JUCE turns into `-lssl -lcrypto`, so a TLS-off Linux build still links
  OpenSSL if libssl-dev is present. Go.dot clears the list outright.
- `XmlPersistence` writes a timestamp header; a canonical mode would let its harness stop
  normalising.
