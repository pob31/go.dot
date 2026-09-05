# Go.dot — Reuse map

**Draft 0.2**, 2026-09-05 (0.1 was 2026-09-04). What the author's other projects already
provide, per Go.dot phase, and what stops each piece being used as-is. Written so the next
phases start from it instead of rediscovering it. Draft 0.2 re-verified the Phase 2 rows
against the checkouts, added what each one is *for* in Phase 2, and corrected one claim of
draft 0.1 in writing rather than by deletion (§ *A correction to draft 0.1*). Paths are relative to the sibling checkouts on the
development machine (`D:\dev\WFS_DIY_v1`, its `spatcore` submodule, `D:\dev\juce_simpleweb`)
and to the repositories `github.com/pob31/{WFS-DIY,spatcore,juce_simpleweb}`.

The PRD is deliberately not amended by this file: `CLAUDE.md` is PRD §4 byte for byte and
CI enforces it, so amendments go into the PRD by the author, citing this map.

---

## The three sources

| Source | What it is | Licence | State |
|---|---|---|---|
| **WFS-DIY** | the shipping wave-field-synthesis app; JUCE 9.0.1, Projucer-built | GPL-3 | production |
| **spatcore** (`WFS_DIY_v1/spatcore`, pinned at `7e7ed63`) | the shared real-time core extracted from WFS-DIY: `rt/`, `dsp/`, `wfs/`, `reverb/`, `gpu/`, `control/{osc,state,mcp}`, `controllers/`, `ui/`, `io/` | GPL-3 (added 2026-09-04, `7e1a8ad`) | consumed by WFS-DIY at source level; CMake targets exist for XOA / Tight-WFS |
| **juce_simpleweb** (`pob31/juce_simpleweb`, a fork of `benkuper/juce_simpleweb`) | JUCE module: HTTP + WebSocket server on one port, WebSocket client; Simple-Web-Server (MIT) over standalone asio (BSL-1.0); TLS optional | GPL-3 | **converged 2026-09-05** on `b953ada` = upstream + the TLS-off guard + the Windows fixes; WFS-DIY, XOA and Tight-WFS vendor the same code |

XOA and Tight-WFS were looked at only for conventions; both are under construction and
nothing here depends on them.

## Two constraints that decide what can be lifted verbatim

1. **JUCE 8.0.13 versus JUCE 9.0.1.** Go.dot is pinned to JUCE 8.0.13 because that is the
   SHA Tracktion Engine 3.5.0 was tested against (`scripts/check-pins.py`); WFS-DIY and
   spatcore are on JUCE 9.0.1. Anything in spatcore that uses a JUCE 9 API does not compile
   here. **Draft 0.1 named one instance and it was wrong** (see the correction under
   *Phase 1* below): `control/osc/OSCParser.h`'s `juce::OSCArgument (true)` compiles on
   both. **No confirmed JUCE-9-only usage in spatcore is known today** — which makes this a
   constraint to keep testing against rather than one with a worked example, and the rule
   below is what to do about it. Everything Go.dot has actually compiled is clean:
   `rt/{RtSnapshot,RtThreadPriority,AudioWorkgroupCoordinator,LockFreeRingBuffer,
   SharedInputRingBuffer}.h` and all of `io/` (JUCE-free, `juce_core`-only, or using
   `juce::AudioWorkgroup`, which is a JUCE **8** feature). spatcore is C++17, so the
   language level never bites either.

   **The rule that survives the correction:** compile a spatcore header in its own
   translation unit before depending on it, rather than reasoning about which JUCE version
   its APIs came from.
2. **One JUCE compile.** `cmake/WfgThirdParty.cmake` compiles every JUCE and Tracktion
   module exactly once, in `wfg_thirdparty`. spatcore's CMake targets link `juce::*`
   module targets, which would compile the module sources a second time into
   `spatcore-*` — so spatcore is consumed at **source level** (as WFS-DIY itself does),
   never through `add_subdirectory(spatcore)`.

## Per phase

### A lesson taken rather than a component

WFS-DIY defines its parameters in per-tab CSVs and generates its MCP tool surface from
them — but its OSC address map and its OSCQuery namespace are hand-written C++, so there
are **three independently-maintained surfaces** kept honest only by a runtime auditor that
logs drift after the fact. `MCPOSCQueryAuditor` exists for that reason, and spatcore's own
boundary proposal recommends collapsing the three into one registry.

Go.dot starts collapsed: [`parameters/godot-parameters.csv`](parameters/godot-parameters.csv)
is the single source for the document schema, the parameter tree, the RELAX NG schema and
the OSCQuery reply. The CSV convention is WFS-DIY's, and a good one; what is not carried
over is the drift.

### Phase 1 — document and tree (this phase)

| Need | Reuse | Status |
|---|---|---|
| HTTP + WebSocket transport for OSCQuery | `juce_simpleweb` (`SimpleWebSocketServer`, `SimpleWebSocketClient`) | **ready** — pin `pob31/juce_simpleweb` at `b953ada` or later; see *Which juce_simpleweb to pin* |
| OSCQuery server shape | `WFS_DIY_v1/Source/Network/OSCQueryServer.{h,cpp}`: HOST_INFO, full tree, attribute queries, LISTEN/IGNORE, 30 ms coalesced binary pushes, PATH_CHANGED, per-IP echo suppression | **pattern only** — the transport shell is generic, the namespace builders are WFS-specific, and its HTTP handler walks the live ValueTree from a worker thread (a race Go.dot's snapshot removes). Go.dot's server is written in-tree behind a namespace-provider seam |
| OSC codec | `spatcore/control/osc/{OSCParser,OSCSerializer}.h` | **not usable**: it drops bundle time tags, cannot serialise `T`/`F`, decodes them as an int32 `1`/`0`, and throws on an unknown tag. Go.dot writes its own full OSC 1.1 codec, shaped so it can be lifted into spatcore. *(Draft 0.1 also called it JUCE-9-only. That was wrong — see the correction below.)* |
| UDP/TCP receivers with sender IP | `spatcore/control/osc/{OSCReceiverWithSenderIP,OSCTCPReceiver}` (raw-data callback path) | **shape reused** (raw datagram + sender IP:port), code not: their legacy path pulls `OSCParser.h` in |
| Origin tagging | `spatcore/control/osc/OscTransportTypes.h` (`OriginTag`, thread-local `OriginTagScope`) | **concept only**: a thread-local cannot survive Go.dot's queue hop; the origin travels on the event |
| Tick-thread priority | `spatcore/rt/RtThreadPriority.h` (MMCSS "Pro Audio" via runtime-loaded avrt, mach time-constraint, SCHED_FIFO; JUCE-free) | **used as-is** |
| Black-box test convention | `WFS_DIY_v1/tools/validation/control-replay/` (`common.py`, `osc_replay.py`, `oscquery_echo_check.py`, `session_roundtrip.py`): stdlib Python, goldens, exit codes 0/1/2/3, launch the app, write over OSC, read back over OSCQuery | **adopted**; the RFC 6455 client in `oscquery_echo_check.py` seeds `tests/blackbox/` |
| XML persistence | `spatcore/control/state/XmlPersistence` | **not usable**: writes a `<!-- Created: -->` timestamp header (which is why WFS-DIY's harness has to normalise) and uses JUCE's default XML formatting; Go.dot's document must be canonical and byte-identical |
| Number formatting | JUCE's `String (double)` and `var (double).toString()` | **not usable, measured**: over 19 993 random doubles the JUCE writer loses 9 214 of them (46%) to a save-and-load round trip, because it stops at fifteen significant digits; its reader is not correctly rounded either, losing about one in four hundred. Go.dot writes with `std::to_chars`. **The reader is the part worth copying carefully**: an `istringstream` imbued with `std::locale::classic()` looks correct, passes on Windows, and silently rejects every subnormal on macOS, because `num_get` is specified to set `failbit` when the conversion sets `errno` and `strtod` sets `ERANGE` on underflow. Go.dot parses with `strtod_l` / `_strtod_l` against a C locale created once per process, and decides on the result — finite, and the whole field consumed — rather than on a stream flag. This is what put the macOS floor at 13.3, and both halves are worth knowing before any sibling project trusts a JUCE-written number to survive a file |

#### A correction to draft 0.1: `OSCArgument(true)` is not a JUCE 9 symbol

Draft 0.1 said spatcore's `control/osc/OSCParser.h` **requires JUCE 9**, because
`OSCParser.h:128,131` construct `juce::OSCArgument (true)` for the `T` and `F` type tags.
Re-checked against both trees on 2026-09-05, that is not so, and the reasoning is written out
rather than quietly deleted because the *conclusion* it supported happens to survive while the
*reason* does not.

`juce::OSCArgument` has **no boolean constructor in either version**. JUCE 8.0.13
(`juce_osc/osc/juce_OSCArgument.h:53,56,59,67,70`) and JUCE 9.0.1 offer exactly the same five:
`int32`, `float`, `const String&`, `MemoryBlock`, `OSCColour`. So `OSCArgument (true)` binds to
the `int32` overload by integral promotion — unambiguously, since promotion beats the
floating-point conversion — and it **compiles on JUCE 8 exactly as it does on JUCE 9**.

What it does is the same on both, and is worse than a build error: a `T` or `F` argument
decodes as an int32 `1` or `0`, so the boolean type is lost silently on every platform WFS-DIY
already ships on. It is a pre-existing semantic defect that travels with the file, not a
version gate — and it stays on the list of things worth a spatcore decision below.

Two things this changes and one it does not. It **does not** change the decision: Go.dot still
writes its own codec, for the three remaining reasons in the row above. It **does** empty the
JUCE 8 / JUCE 9 constraint of its only worked example — no confirmed JUCE-9-only usage in
spatcore is known today, so §2 above now says to compile a header rather than to reason about
its APIs. And it means anyone porting `OSCParser.h` to JUCE 8 should expect it to build and
then be wrong, which is the harder failure to notice.

### What WFS-DIY's OSCQuery reply actually looks like — read 2026-09-06

Read out of `Source/Network/OSCQueryServer.cpp` for PR 1.6's fixture, and worth keeping
because Phase 2's OSCQuery **client** will parse exactly this.

- **Keys, in the order it emits them.** A container: `FULL_PATH, ACCESS, DESCRIPTION,
  CONTENTS`. A parameter: `FULL_PATH, TYPE, ACCESS, VALUE, [RANGE, CLIPMODE,] DESCRIPTION`.
  `CLIPMODE` is always `["both"]` and appears only alongside `RANGE`. `RANGE` is an array of
  one object with `MIN` and `MAX` — never `VALS`. **There is no `UNIT` key anywhere**, even
  though the documentation CSVs carry units.
- **`ACCESS` takes three values only**: `0` on containers, `3` on parameters, and `1` on
  exactly one node in the whole tree, `/wfs/input/<n>/channelType`. It never emits `2`.
- **The address space is `/wfs/input|output|reverb|config`, channel-indexed.** Source
  position is three scalar nodes (`positionX`, `positionY`, `positionZ`) — there is no
  vector node, no `/source/…`, and no `xyz`. Input children are keyed by the PERMANENT
  channel number, so `1, 2, 3, 7, 12` is a legitimate capture after deletions.
- **EQ band nodes deviate from the spec**: `TYPE "if"`, no `VALUE`, and a two-entry `RANGE`
  (band index, then value). Go.dot keeps entry zero, which is the band index — correct,
  since `RANGE` is per argument.
- **Floats widen to double on the way out**, so `0.1f` serialises as `0.100000001490116`.
  That is the bound the target really enforces, and a reader that rounded it to `0.1` would
  have Go.dot enforcing a bound nobody declared.
- **Not published at all**: `masterLevel`, `speedOfSound`, `temperature`, `haasEffect`, and
  the whole of `/wfs/cluster` and `/wfs/network`. There is no global or master container.
- `HOST_INFO` is a separate `?HOST_INFO` query, not part of `GET /`. It carries `NAME`,
  `OSC_PORT`, `OSC_TRANSPORT "UDP"`, `WS_PORT` (the same port as HTTP) and an `EXTENSIONS`
  object of booleans.

### JUCE's JSON parser cannot be trusted with a number — measured 2026-09-06

`juce_JSON.cpp`'s `parseNumber` accumulates a plain integer literal into an `int64`, one
digit at a time, and only abandons that for the correctly-rounded `readDoubleValue` path
when it meets a `.` or an `e`. A literal with neither, and more digits than an `int64` holds,
**overflows in silence**. Measured over 19 993 random doubles written in their shortest
round-trip form: **142 came back as a different number**, one in 140. The first was
`-40595640456200454144`, which returned as `-3702152308781350912`.

Go.dot therefore has its own reader (`src/wfg/engine/json/JsonValue.h`), which measures the
token out by JSON's grammar and hands it to the same `strtod`-in-a-C-locale conversion the
document, the log and the OSC atoms already use. The test that found this keeps both halves,
so if JUCE ever fixes `parseNumber` the build says so and the question can be reopened.

Worth knowing in WFS-DIY and spatcore too: both parse OSCQuery replies with `juce::JSON`.
The values at risk are range bounds and any large integer a target reports.

### Which juce_simpleweb to pin — settled 2026-09-05

**Pin `pob31/juce_simpleweb` master at `b953ada` or later.** It is upstream master plus two
commits, and every copy in the family now holds the same code.

**The fork is the answer, not a stopgap, and it is the answer FOR EVERY APP IN THE FAMILY**
(author, 2026-09-05: stick with the fork of juce_simpleweb for all apps until further
notice). Go.dot, WFS-DIY, XOA and Tight-WFS all vendor `pob31/juce_simpleweb`, and there is
no plan for any of them to move back to upstream.

The TLS-off guard is going to Ben as a pull request, but that is the author's to send and
its outcome changes nothing here: if it is merged, pinning upstream instead becomes possible
and optional; if it is not, the fork carries it indefinitely and nothing breaks. Either way
PR 1.D pins the fork, and this is not a decision waiting on anybody.

How it got there, because the starting position was worse than it looked. There were five
trees and none was a superset of the others: upstream `benkuper/juce_simpleweb` had nine
commits nobody else had, while WFS-DIY, XOA and Tight-WFS vendored byte-identical copies of a
private lineage — old upstream plus three Windows/TLS patches that existed in no git repository at
all. The fork itself was simply nine commits behind and carried none of the patches.

The fork turned out to need a fast-forward rather than a rebase: it had zero commits
upstream lacked, because its one contribution (the OPTIONS fix) had already been merged as
upstream PR #5. On top of that go two commits carrying what only the vendored copies had:

- **the TLS-off guard** — `#ifndef SIMPLEWEB_SECURE_SUPPORTED` around the define. Without
  it a consumer passing `-DSIMPLEWEB_SECURE_SUPPORTED=0` gets a macro redefinition and TLS
  anyway, so spatcore's recipe was configuring a header that overrode it. This is the one
  Go.dot needs, and it is backwards-compatible for everyone who does not set the macro —
  the obvious pull request to Ben, which the author is sending. Go.dot does not wait on it.
- **the Windows fixes** — `NOGDI` no longer defined, since it hides `LOGFONTW` and `RGBQUAD`
  from any consumer that also links a JUCE GUI module; and `_WIN32_WINNT 0x0A00` set before
  anything reaches `<winsock2.h>`, unconditionally rather than only in the TLS branch, where
  a TLS-off build never saw it.

What upstream brought that the family did not have, and the reason this was worth doing now
rather than at Phase 1.9: **a deadlock when stopping the server while it waited for the
MessageManager lock** — a hang at shutdown, which in a theatre means during get-out. Also a
WebSocket handshake key sometimes computed wrong and refused by some clients, an error code
passed to `connectionError`, and a request timeout on the client.

**One signature changed**, and Go.dot should write against the new one from its first line:
`connectionError` now carries an `int status` before the message. WFS-DIY's two overrides
moved with it. Because both were marked `override`, the mismatch was a compile error rather
than a callback that silently stopped being called.

Verified before pushing: the module compiles both TLS-off and at its default alongside
`juce_gui_basics`; WFS-DIY rebuilt Debug x64 clean; XOA and Tight-WFS rebuilt clean; the
plugin's `OscQueryClient` compiles against the new signature.

### Phase 2 — first sound (the next phase)

Re-verified against the sibling checkouts on 2026-09-05. Every header in this table
**compiles on JUCE 8.0.13**: the JUCE 8 / JUCE 9 boundary of §2 turns out to bind
`control/osc` only, and even there not for the reason draft 0.1 gave. spatcore targets
C++17, so nothing in it needs a language-level exception either.

| Need | Reuse | Status |
|---|---|---|
| Device open policy | `spatcore/io/DeviceHost.h`: `AudioDeviceManager::initialise (max, max, xml, …)`, explicit `BigInteger` masks with `useDefaultInputChannels` / `useDefaultOutputChannels` set **false** (while either is true, `setAudioDeviceSetup` throws the caller's mask away and substitutes the count frozen at the last `initialise`), `ensureDeviceTypesScanned()` before `setAudioDeviceSetup` (on an unscanned manager it takes a no-device-type early exit and returns an *empty* error string while opening nothing), and read-back of the *active* masks rather than the requested ones | **used as-is**, at source level |
| Hardware-indexed callback | `spatcore/io/DeviceIoCallback.h`: `audioDeviceIOCallbackWithContext`, buffer channel *h* == hardware channel *h*, device output rows aliased zero-copy, everything sized in `audioDeviceAboutToStart`, no allocation and no lock in the callback. Avoids `AudioSourcePlayer`, whose `float* channels[128]` caps a buffer at 128 channels | **shape used as-is; one seam differs.** Its per-block hook is a `juce::AudioSource` (`getNextAudioBlock`), while Tracktion's hosted interface wants `processBlock (AudioBuffer<float>&, MidiBuffer&)`. Go.dot's callback calls the hosted interface directly rather than dressing it as an `AudioSource` |
| Gain slew | WFS-DIY `Source/MainComponent`: `juce::SmoothedValue<float, ValueSmoothingTypes::Linear>` — **never Multiplicative**, which cannot ramp to or from zero and emits a NaN burst, i.e. a loud crack on the PA, guarded in JUCE by nothing but a debug `jassert`; `std::atomic<float>` targets; a 50 ms ramp; `setTargetValue` once per block; per-sample `getNextValue()` only while `isSmoothing()`, else the vectorised `applyGain`, skipped entirely at unity | **adopted verbatim** in `CueOutputPlugin` |
| Message → real-time hand-off | `spatcore/rt/RtSnapshot.h` — a POD snapshot (`static_assert (is_trivially_copyable_v<T>)`), but under a `juce::SpinLock` on **both** sides; spatcore's own `effects-channels-plan.md` already recommends a triple buffer for new work, and `RtTripleBuffer.h` does not exist yet | **concept only.** Go.dot's audio-side state is per-coefficient atomics (level, routing matrix), which needs no snapshot at all; if a struct hand-off does appear, it is written as the lock-free triple buffer spatcore's plan asks for, so it can be lifted back |
| Tick-thread priority | `spatcore/rt/RtThreadPriority.h` — JUCE-free; MMCSS "Pro Audio" through a runtime-loaded `avrt.dll` (no import library on any link line), mach time-constraint policy, `SCHED_FIFO` | **used as-is** (already Phase 1's) |
| Apple Silicon audio workgroups | `spatcore/rt/AudioWorkgroupCoordinator.h` — wraps `juce::AudioWorkgroup::join (WorkgroupToken&)`, which are **JUCE 8** features, not JUCE 9; one `generation` atomic on the fast path, a `CriticalSection` only when the workgroup changes. Tracktion's own switch is `EditPlaybackContext::enableAudioWorkgroup (true)`, macOS-only and marked experimental | **deferred, not rejected.** The M4 Pro cross-check left workgroups off and still found reproducibility perfect, so this is now a question about xrun headroom at buffer 32, not about correctness |
| Fade shapes | Two conventions exist in WFS-DIY and neither is Go.dot's. `Source/Sampler/SamplerEngine.h` counts fade-in/out in integer samples on the audio thread. `Source/Network/OSCParameterRamper.h` ramps ValueTree parameters at 50 Hz on the message thread, takes its progress from **wall-clock elapsed** because GUI congestion stretched its 20 ms tick past 100 ms, and drops a ramp if anyone else wrote the parameter meanwhile (takeover detection) | **takeover detection adopted; wall-clock progress deliberately not.** Go.dot's `FadeJob` counts *ticks*, which is what lets a fade replay identically. `SamplerEngine` also carries a trap worth naming: it copies a `std::shared_ptr<AudioBuffer>` on the audio thread, so the last reference can be dropped — and the buffer freed — inside the callback |
| Lock-free rings | `spatcore/rt/LockFreeRingBuffer.h` (SPSC float ring), `rt/SharedInputRingBuffer.h` (single-producer, multi-consumer, each consumer owning its own cursor; a slow consumer is overrun by design, there is no backpressure) | **not needed in Phase 2** — no live input yet. Phase 9's rack will want both |
| Protective ramp on a test signal | `spatcore/io/TestSignalGenerator.h` — a 500 ms ramp declared a safety property rather than a nicety, plus a 10 ms declick, and it *replaces* rather than mixes into its target channel | **not needed in Phase 2**; recorded for whenever Go.dot grows a test tone |
| The observed sample rate | WFS-DIY reads it only from the live device (`device->getCurrentSampleRate()`, 48 kHz fallback when none is open) and **never exposes it over OSCQuery** — its namespace is built by walking the ValueTree, which holds no audio-device node, so the rate reaches no client at all | **shape reused, scope widened.** Go.dot publishes rate, block size and clock source under `/godot/engine` and `/godot/audio`, because §6.2 makes "no clock" and "no interface" two failures an operator has to tell apart at a glance |
| RT-safety instrumentation | — | **none exists** anywhere in the author's repos: no malloc hook, no `operator new` override, no `-fsanitize=realtime`, no allocation counter. The only instrument is `JUCE_ASSERT_MESSAGE_THREAD`, asserted at the *non*-real-time end of each seam. Go.dot's PR 2.2 is new work, and the first place §4.2's lipogram is enforced by anything other than review |

### Phase 2/4 — closed-loop cues and mock targets

| Need | Reuse | Status |
|---|---|---|
| An OSCQuery *client* | `WFS_DIY_v1/Plugin/Source/Shared/OscQueryClient.{h,cpp}`: HTTP over a raw `juce::StreamingSocket` **because `juce::URL` re-encodes OSCQuery's bare `?HOST_INFO` as `?HOST_INFO=`, which the server then does not match**; WebSocket through juce_simpleweb; `LISTEN`/`IGNORE` as JSON commands; and the detail that shapes Go.dot's design — **the server sends no current value on `LISTEN`**, so the client polls `?VALUE` once per subscribed path after connecting | **pattern adopted, code rewritten.** Its own OSC decode handles only `f i T F` and coerces all four to `float`; Go.dot has a full OSC 1.1 codec already. Its callbacks arrive on the WebSocket thread with no marshalling, deliberately — an async hop could outlive the owning processor during a plugin host's scan |
| `verified` read-back | the same client, polling `?VALUE` once per tick | **this is why Phase 2 verifies by polling** rather than by `LISTEN`: it is what WFS-DIY's own client had to do, and it leaves no subscription state to reconstruct when the log is replayed with no network |
| OSC over TCP | `spatcore/control/osc/OSCTCPReceiver` — 4-byte big-endian length prefix, at most 16 clients, one thread each; its `.cpp` pulls `OSCParser.h` in | **not used in Phase 2** (mounts send UDP; 8011 stays reserved). The framing is recorded for when `verified` wants TCP |
| Mock target | — | **none exists.** `tools/validation/control-replay/remote_tablet_mock.py` is a mock *client*, and nothing in `tools/` fakes a server or a device. Go.dot writes `tests/blackbox/mock_target.py` (stdlib HTTP + UDP) plus an in-process scripted target behind its own namespace-provider seam |
| Golden drivers | `tools/validation/control-replay/common.py`: exit codes 0 pass / 1 mismatch / 2 usage / 3 app-start, an `App` launcher that polls for readiness, `OSCSender`, `oscquery_get`, `compare_or_update` | **adopted** (already Phase 1's convention) |

### Phase 3 — groups, triggers, ranges

Nothing to reuse: no sibling project has a cue model, which is the whole reason Go.dot
exists. What Phase 3 does have waiting for it is a decision already recorded rather than
one to rediscover — **group waits compose with member waits rather than replacing them**
(author, 2026-09-05). A group's pre-wait runs before its members begin theirs, so raising
one number defers a whole scene without disturbing relative timing; a group's post-wait
runs after every member is complete, members' own post-waits included. The full statement,
including how it stacks through nested groups, is in
[`godot-namespace-draft-0.1.md`](godot-namespace-draft-0.1.md) §2.4, *Waits compose*.

The schema already carries it: `preWait` and `postWait` are `cue` rows, and a Group
inherits every Cue row, so a group has its own from Phase 1.2 onward.

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

- `control/osc/OSCParser.h` / `OSCSerializer.h`: bundle time tags parsed and discarded,
  always written as "immediately"; `T`/`F` arguments serialise to nothing (a desync) and
  decode as an int32 `1`/`0`, because `OSCArgument (true)` promotes to the `int32` overload —
  on **both** JUCE 8 and JUCE 9, so this is a live defect in WFS-DIY today rather than the
  version gate draft 0.1 called it. Go.dot's codec could be lifted into spatcore.
- ~~The juce_simpleweb lineages have diverged in both directions.~~ **Closed 2026-09-05**:
  all five trees hold `b953ada`, and Go.dot pins the fork. A pull request to Ben for the
  TLS-off guard is with the author; if it lands, pinning upstream directly becomes an option
  rather than a need.

- `SpatcoreConsumer.cmake` strips `libssl libcrypto z` from juce_simpleweb's interface
  link libraries — the macOS/Windows spellings. The module also declares `linuxLibs:
  ssl,crypto`, which JUCE turns into `-lssl -lcrypto`, so a TLS-off Linux build still links
  OpenSSL if libssl-dev is present. Go.dot clears the list outright.
- `XmlPersistence` writes a timestamp header; a canonical mode would let its harness stop
  normalising.
