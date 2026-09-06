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
| HTTP + WebSocket transport for OSCQuery | `juce_simpleweb` (`SimpleWebSocketServer`, `SimpleWebSocketClient`) | **pinned at `b72ec94` since PR 1.10**, and proven on all three platforms by `SimpleWebToolchainTests.cpp`. One defect found and fixed upstream doing it: `start(0)` could not report its bound port — see below |
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
  `CLIPMODE` is always `["both"]` and appears only alongside `RANGE` — but not every `RANGE`
  brings one: *measured on the capture*, 2 032 scalar parameters carry both, while the 170
  band nodes carry `RANGE` and neither `CLIPMODE` nor `VALUE`. `RANGE` is an array of
  one object with `MIN` and `MAX` — never `VALS`. **There is no `UNIT` key anywhere**, even
  though the documentation CSVs carry units.
- **`ACCESS` takes three values only**: `0` on containers, `3` on parameters, and `1` on
  `/wfs/input/<n>/channelType` — which is one node per input channel, so *fourteen* of them
  in the committed capture rather than one. It never emits `2`, so **nothing mounted from
  WFS-DIY is ever inferred as an event**.
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
  Confirmed against the capture, which adds `irFile` to that list and finds nine
  `/wfs/config/stage` parameters published that no table documents — see *What a real
  WFS-DIY namespace turned out to be* below.
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

### juce_simpleweb could not bind an ephemeral port — found in PR 1.D, fixed in PR 1.10

**Fixed upstream at `b72ec94`, and Go.dot is pinned there.** The account below is kept
because the failure mode is worth recognising again, and because the same comparison
appears in other people's start callbacks.

`SimpleWebSocketServer::start(0)` used to bind successfully and then be unable to tell you
what it bound. Confirmed by test, not by reading: `SimpleWebToolchainTests.cpp` pinned the
broken behaviour with a note saying it would go red the day somebody fixed it. It did, and
that test now asserts the opposite - it is the check that the fix is really in the pinned
submodule, so a bad re-pin fails there rather than in the black-box harness.

`SimpleWebSocketServer.cpp:374-377`:

```cpp
void SimpleWebSocketServer::httpStartCallback (unsigned short _port)
{
    isConnected = port == _port;
}
```

Simple-Web-Server hands that callback the port asio **actually bound**. `port` is the one
the caller **asked for**. For a fixed port the two agree and nothing is wrong. For port 0 —
the ephemeral request — they never agree, so `isConnected` stays `false` for the life of a
server that is in fact listening, and the one number anybody needs in order to reach it is
discarded.

**The fix was two lines, applied to both servers:**

```cpp
void SimpleWebSocketServer::httpStartCallback (unsigned short _port)
{
    port = _port;
    isConnected = true;
}
```

Which is what the comparison already meant for a fixed port, and the only way to learn the
answer for an ephemeral one. `SecureWebSocketServer` carried the identical bug at `:666`
and got the identical fix.

**It was never a test-only inconvenience.** `wfg serve --http-port=0` prints its bound port
so the black-box harness can drive it, and binding 0 is how two Go.dot instances coexist on
one machine. PR 1.D shipped with a workaround — borrow a port from the OS, release it, hand
the number to the server — which carried a real race between the release and the bind. That
workaround is gone.

**It affected every app in the family**, since all four vendor the same code, and none had
noticed because all four pass a fixed port from configuration. WFS-DIY, XOA and Tight-WFS
get the fix by moving to `b72ec94` whenever they next re-pin.

### Which juce_simpleweb to pin — settled 2026-09-05

**Pin `pob31/juce_simpleweb` master at `b72ec94` or later.** It is upstream master plus
three commits: the TLS-off guard, the Windows fixes, and the ephemeral-port fix below.
Go.dot is on `b72ec94`; the other three apps hold `b953ada` until they next re-pin, and
inherit the port fix when they do.

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
| Device open policy | `spatcore/io/DeviceHost.h`: `AudioDeviceManager::initialise (max, max, xml, …)`, explicit `BigInteger` masks with `useDefaultInputChannels` / `useDefaultOutputChannels` set **false** (while either is true, `setAudioDeviceSetup` throws the caller's mask away and substitutes the count frozen at the last `initialise`), `ensureDeviceTypesScanned()` before `setAudioDeviceSetup` (on an unscanned manager it takes a no-device-type early exit and returns an *empty* error string while opening nothing), and read-back of the *active* masks rather than the requested ones | **policy adopted, two bugs found in the seam (PR 2.7).** The explicit-mask half is used as written and is the part worth having. Two assumptions did not survive Go.dot's use: `openNamedDevice` needs an INITIALISED manager, or it answers "No such device" for a device the enumeration just listed (spatcore's own consumers restore from saved state on launch and never take that path); and `setDeviceAllChannels` names the device as an INPUT as well as an output, which is right for an RME and wrong for a laptop's speakers, whose lookup then happens in an empty list. Go.dot initialises first and opens OUTPUT-ONLY, which is also what a commanded player wants (§3.25) and stops it holding an input somebody else needs |
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

### What a real WFS-DIY namespace turned out to be (captured 2026-09-05)

The `/wfs` mount fixture is no longer a hand-written stand-in: it is
`GET http://127.0.0.1:5005/wfs` from a running WFS-DIY, committed at
`tests/fixtures/bundles/minimal/namespaces/wfs-diy.json` with its provenance in
`tests/fixtures/README.md`. Session: 12 mono + 2 stereo inputs, 24 outputs, 9 reverbs.
**2 480 nodes, 1.04 MB, nesting depth 3**, and Go.dot's mount reader accepts it unmodified —
every `FULL_PATH` consistent, no duplicate address, no unusable node name.

What it costs, measured by replacing a 17-node placeholder with it: the unit suite goes from
**1.2 s to 4.2 s** and the whole suite from **3.2 s to 9.9 s**. Most of that is re-parsing a
megabyte of JSON once per mount test case, not the parameter tree's rebuild.

**Shape.** 54 containers, 2 426 states, **no events** — `ACCESS` is 0 on containers and 3 on
parameters, with exactly one exception: `channelType`, published read-only, one per input.
Type tags are `f` (1 107), `i` (1 065), `s` (84) and `if` (170); the `if` nodes are the EQ
bands, whose `RANGE` carries one entry per argument, band index first. No `UNIT` key and no
`VALS` enumeration appear anywhere in the tree.

**Three things the tables have that the server does not send.** This is the useful half of
the tables, and each one is a thing WFS-DIY could fix at its own end rather than something
Go.dot should work around:

1. **Descriptions.** `DESCRIPTION` on a parameter is its own leaf name — `"attenuation"` —
   for 2 256 of 2 426 leaves; only the 170 band parameters differ, and only by a synthesised
   `" (first arg: band index 0-5)"`. The tables carry real prose for the same parameters
   (`"Input Channel Attenuation."`) and it is already written. Sending the hover text as
   `DESCRIPTION` would cost nothing and improve every OSCQuery client.
2. **Enumerations.** An enumerated parameter is published as a bare integer span. `algoType`
   is `MIN 0, MAX 2`; the tables say `SDN ; FDN ; IR`. A client can set it and cannot name it.
3. **Units.** Seventeen distinct units in the tables, none in the tree.

**The reverb models: measured, because the shape might have depended on them.** It does not.
Three captures taken with the algorithm switched to SDN, FDN and IR are **identical at 2 480
nodes** — nothing added, nothing removed, and not one `TYPE`, `ACCESS` or `RANGE` changed.
Exactly one value differs between them, `/wfs/config/reverb/algoType`. So **one capture covers
all three models**, which is what makes a single committed fixture legitimate.

The interesting part is what that means for a client. The UI hides most of the panel when the
model changes — under IR it drops RT60, both crossovers, diffusion and the size control
altogether and shows an IR file chooser instead — but **OSCQuery keeps publishing all 29
reverb parameters as writable with full ranges regardless**. `sdnScale` and `fdnSize` are two
always-present nodes of which at most one is ever live. A client can write `rt60` while IR is
selected, and nothing in the description says it will do nothing. The tables know
(`Visible only for SDN`, `Visible only for FDN`, `Visible only for IR`); the namespace does
not. Likewise `irLength` and `irTrim` publish their hard caps of 30 s and 30 000 ms, while
the tables record that the effective maximum is the loaded file's duration — a constraint no
client can see.

**What the capture publishes that the tables do not, and the reverse.** Compared leaf by leaf
per family, against the 265 `/wfs` rows in `Documentation/*.csv`:

| family | published | in the tables | documented, not published | published, not documented |
|---|---|---|---|---|
| `input` | 111 | 142 | **39** | 8 |
| `output` | 25 | 28 | 4 | 1 |
| `reverb` (per channel) | 26 | 30 | 4 | 0 |
| `config/reverb` | 29 | 30 | 1 | 0 |
| `config/stage` | 9 | 0 | 0 | **9** |
| `cluster` | **0** | 34 | **34** | 0 |

- **The whole `/wfs/cluster` family — 34 addresses — is documented and not published at all.**
  Clusters are invisible to any OSCQuery client, so nothing Go.dot mounts can reach them.
- The 39 unpublished input addresses are mostly one feature: **24 of them are the Sampler**
  (per-cell and per-set), plus the otomo transport and the map-visibility controls.
- **`irFile` is documented and not published**, so the impulse response cannot be chosen over
  the network — the IR panel's only real control is unreachable.
- **`config/stage` is published and undocumented** — nine geometry parameters in the tree that
  no table mentions.
- Three **rename drifts**, where both sides mean the same parameter: the tables say
  `FRactive`, `LFOactive`, `LSactive` and `samplerActiveSet`; the server publishes `FRenable`,
  `LFOenable`, `LSenable` and `samplerSet`. And one **casing typo**: the tables' `Eqfreq`
  against the server's `EQfreq`.

None of this is Go.dot's to fix, and none of it blocks anything: it is what `MCPOSCQueryAuditor`
exists to catch, measured once from the outside. For Go.dot the operative facts are that the
capture is honest about a narrower surface than the application has, and that a Phase 2
`verified` cue can only target what is actually published.

### The world that does not speak OSCQuery — surveyed 2026-09-05

PRD §3.22 says a device that speaks OSCQuery describes itself, and that templates are only
needed for devices that cannot. **The author's correction, and it inverts the emphasis: the
devices that cannot are the normal case.** OSCQuery was never standardised, so most vendors
have not implemented it and will not until there is a final specification to implement.
WFS-DIY is capturable only because the same person wrote both ends. Everything else Go.dot
mounts will be hand-written, and at best its values can be asked for over OSC by some
device-specific convention.

The format decision is untouched by this — a hand-written and a captured description are the
same file and the engine cannot tell them apart, which is exactly what §3.22 bought. What
changes is where descriptions come from, and what `verified` (§3.11) can mean.

**Chataigne is the place to look**, because it has solved this for a decade in the open: its
modules are hand-written device definitions, and its author, Ben Kuperberg, also wrote the
`juce_simpleweb` module Go.dot pins for its own HTTP and WebSocket transport. The community
index (`benkuper/Chataigne-community-modules`) lists **108 modules by 44 authors**, eleven of
them Ben's. 87 were readable when surveyed; **21 of the 108 URLs no longer fetch**, which is
worth knowing before anyone plans to harvest them.

#### What a Chataigne module is, and how it differs from an OSCQuery node

Three disjoint parts. `parameters` configure the module itself. `commands` are outgoing
actions, each a name with typed arguments and a callback into a script. `values` are a
separate tree of incoming feedback. **The two trees need not correspond at all.**

This is the important structural difference. An OSCQuery node is one address that is both
readable and writable, carrying `ACCESS`. A Chataigne module has things you can *do* and,
separately, things you may *learn* — and most devices really are shaped that way. WFS-DIY's
own documentation tables have the same shape (`/wfs/output/attenuation <ID> <value>` is a
call signature, not a node), which is why they could not be converted into a namespace.

Commands also take the object index as an **argument**, not a path segment: all 35 ADM-OSC
commands take a `Source index`, grandMA3 takes `Page`, `Executor` and `Offset`. Go.dot's
reader already keeps `RANGE` per argument for multi-argument nodes, so the machinery half
exists — but whoever hand-writes a template must decide whether an indexed family becomes
N nodes or one node with an index argument, and that choice decides what `verified` can mean
for it.

#### Four ways the ecosystem gets a value back, and three of them are the author's work

| mechanism | seen in | what it costs |
|---|---|---|
| **Self-description and query** — ask any node | OSCQuery targets only: WFS-DIY, ossia | nothing; this is the case Go.dot already implements |
| **A protocol get-convention, polled** | **ADM-OSC**: 35 commands, 3 static feedback values, and module *parameters* that are polling toggles — "Get Sound Objects positions XYZ", "Get gain", "Get mute", plus a "Get update Rate" | the module must know the convention and run a poll loop |
| **Subscribe, then listen** | **ETC Eos**: six feedback values, populated because the console pushes after `/eos/subscribe` | the device must offer it |
| **A bespoke sync command** | **grandMA3**: "Add Executor to SyncList" and "Sync Executors", written by the module author because the console will not volunteer state | invented per device |
| **Nothing** | 32 of 87 modules declare no feedback values at all; 11 more declare output without input | state is tracked locally or not at all |

#### The numbers, and one thing they do not say

- **Protocols**: OSC 35, MIDI 19, WebSocket 9, TCP 7, HTTP 5, Serial 5, then a tail. OSC is
  40% of what Chataigne covers; Go.dot's mount model is OSC-shaped.
- **Direction**: 71 modules both send and receive, **11 send without receiving**, 3 receive
  without sending, 2 do neither.
- **Every one of the 87 has a script.** Not one module in the sample is purely declarative.
  Whatever Go.dot's template format is, the ecosystem's evidence is that the static half is
  never sufficient by itself.
- In aggregate there are more feedback values (5 526) than commands (1 822), but that is
  skewed and should not be read as "feedback is common": **SPAT Revolution alone contributes
  3 215**, and MIDI control surfaces are feedback-heavy because every button reports — which
  is input arriving, not device state that can confirm a write.

**The closest analogue to what Go.dot mounts is in there.** Flux's **SPAT Revolution** module
is OSC, 3 215 feedback values, 102 commands — and it is hand-enumerated longhand, "Source 1",
"Source 2", each with its own parameter block, and the same for rooms. That is the
hand-written equivalent of what WFS-DIY's server generates. It also puts the 2 480-node
WFS-DIY capture in proportion: that is an ordinary size for a real spatial-processor
template, not an outlier.

#### What Go.dot should take from it

- **A mount must be able to declare what it can do.** Chataigne solves this with two booleans,
  `hasInput` and `hasOutput`. Go.dot's mount declares `transport`, which says how to *send*,
  and nothing about whether the target can be *asked*. So a cue with `wait: verified` against
  a write-only target is a cue that cannot succeed, and today nothing would notice until the
  show. This is question K in the namespace draft.
- **`verified` needs designing against four mechanisms, not one.** Phase 2 implements the
  OSCQuery one, which covers the author's own processors — which is what PRD §3.11 already
  scopes it to when it says "default for own processors". The other three are Phase 4 or
  later, and the mount capability is what keeps the difference honest in the meantime.
- **PRD §3.11's opening premise, "Targets speak OSCQuery", overstates what is true.** Its own
  parenthesis already scopes the mechanism correctly. An amendment is the author's to make;
  it is recorded here rather than edited there.
- **Harvesting Chataigne or Bitfocus Companion definitions later is realistic**, and the
  conversion is a generator job of the shape `scripts/generate-schema.py` already models: a
  committed output plus a `--check` drift gate. Two cautions. Each module lives in its own
  repository under its own licence, so it is a per-module check rather than one decision. And
  deriving a namespace from a *published protocol specification* — as ADM-OSC is, and as PRD
  §3.22 says ships built in — is a cleaner path than copying a module that implements it.

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
