# Handoff — the macOS-only OSCQuery failures

**Written 2026-09-05 from the Windows box, at `25e1d8a`.** Everything below was established
without a Mac, by reading the code and the CI logs. It has a prime suspect with a verified
mechanism, two hypotheses that were investigated and **refuted** (do not re-run them), and a
first move that **may not need the Mac at all**.

Read §2 and §3 before touching anything.

---

## 1. What you are picking up

`main` is green on Linux (including the strict GCC `-Werror` build), Windows, the spikes job
and the pins gate. **macOS is the only red platform**, and has been since the Phase 2 audio
tests landed.

Two ctest entries fail there — `unit.C` and `unit.fr_FR`, the same doctest binary under two
locales. The other 22 entries pass. The failures are all in `tests/OscQueryTests.cpp`, which
is **Phase 1 code and has not been touched**. What changed is that ~39 audio cases now run in
the same process first.

---

## 2. The prime suspect: `boundPort()` latches **0**

Verified by reading the tree. Every file:line below was checked, not inferred.

### The mechanism

`SimpleWebSocketServerBase` publishes two plain, **non-atomic** members
(`ThirdParty/juce_simpleweb/SimpleWebSocketServer.h:34,37`):

```cpp
int port;
bool isConnected;
```

`initServer()` hands asio an **external** io_service (`SimpleWebSocketServer.cpp:219`), so
`server_http.hpp` leaves `internal_io_service` false (`:412`). `ServerBase::start` therefore
binds and listens synchronously, but only **posts** the callback that reports the granted
port (`server_http.hpp:436-451`) — the whole `if (internal_io_service)` run block at `:453`
is skipped. Then, back in `SimpleWebSocketServer.cpp`:

```cpp
http->start (std::bind (&SimpleWebSocketServer::httpStartCallback, this, _1));

DBG("Service run");
isConnected = true;          //  <-- line 251. TOO EARLY.
isConnecting = false;

webSocketListeners.call (&Listener::serverInitSuccess);

if (ioService != nullptr)
    ioService->run();        //  <-- line 258. Only NOW does the posted callback run.
```

and the posted callback, which is the **only** thing that ever writes the real port, is at
`SimpleWebSocketServer.cpp:386-387`:

```cpp
port = _port;
isConnected = true;
```

Meanwhile the reader, `OscQueryServer::start`
(`src/wfg/engine/oscquery/OscQueryServer.cpp:311-332`):

```cpp
impl->server.start (portToBind);

/*  isConnected is set from the server's own thread once asio has bound. */   // <- WRONG
for (int i = 0; i < 2000 && ! impl->server.isConnected; ++i)
    juce::Thread::sleep (5);
...
port.store (impl->server.port, std::memory_order_relaxed);
```

**If the poll observes the line-251 write rather than the line-387 write, it reads `port`
while it is still 0.** That comment in our own code is the bug in one sentence: it believes
`isConnected` means "bound", and there are two writers and only one of them means that.

### Why that produces exactly what CI shows

- `start()` returns **true**, so `REQUIRE (rig.started)` **passes** — which is why it has
  never once appeared in the failure list, at `OscQueryTests.cpp:435/510/528/587/662/737/782/816`.
  Any hypothesis that cannot explain that is wrong. This one explains it precisely.
- `get()` then calls `juce::StreamingSocket::connect ("127.0.0.1", 0, 10000)`
  (`OscQueryTests.cpp:244`). Port 0 fails `isValidPortNumber` (`juce_Socket.cpp:80-83, 562`),
  connect returns false, and `get()` returns the default `HttpReply` — **`status == 0`**.
  That is the observed `values: CHECK( 0 == 200 )` and `CHECK( 0 == 400 )`. **Not a wrong
  status code. No connection at all.**
- The WebSocket client builds `"127.0.0.1:0"` (`OscQueryTests.cpp:334`), never opens, and
  `waitUntil` burns its full **10-second** timeout — line 591.

### The clustering, which is the strongest evidence and which I missed at first

doctest re-enters the whole `TEST_CASE` body once per `SUBCASE`, so each subcase builds its
**own** `Rig` on its own port (`OscQueryTests.cpp:339-349`). The observed failing lines fall
into per-subcase clumps:

```
388 + 393                        one rig
441 + 444                        one rig
458                              one rig
466, 467, 470, 471, 477          one rig
483 + 484                        one rig
591                              one rig
```

That is **one poisoned rig taking all of its own requests down**, not random per-request
flakiness. It is what a latched bad port looks like and it is not what resource exhaustion
looks like.

### Why macOS

The race is present on every platform — it is a program-order bug, not just a memory-model
one. macOS loses it because `macos-latest` is **arm64** (`ci.yml:323` says so in its own
comment) while the Linux and Windows runners are x86-64, and those arm64 runners are small
(3 cores) with efficiency cores and QoS bands. The server thread is a `juce::Thread` at
`Priority::normal` → `QOS_CLASS_DEFAULT`. Under load it can be parked long enough that the
reader's very first 5 ms poll lands in the window between line 251 and the dispatch of the
posted callback.

**Honest gap:** *why the audio tests specifically tip it* is not established. The likeliest
answer is plain machine load and thread contention — they take ~30 s and construct ~20
Tracktion engines. Two more interesting explanations were tested and both failed (§3). If you
confirm §2 and fix it, this gap stops mattering; if you cannot, it is the thread to pull.

---

## 3. Refuted — do not spend Mac time on these

### 3a. File-descriptor exhaustion — REFUTED

This was my own first hypothesis and it is wrong. It is seductive because it explains
macOS-only (macOS ships a low default `ulimit -n`), non-determinism, and liveness in one
sentence. It does not survive contact:

- **Exhaustion is monotone.** Once over the ceiling the server's own descriptors fail too, so
  `REQUIRE (rig.started)` would redden. It never has — including in the run where line 591
  failed under a rig that had started fine at line 587.
- **`tests/SimpleWebToolchainTests.cpp` does the identical socket work and sorts *after*
  `OscQueryTests.cpp`**, so it runs under strictly more pressure. It has never failed.
- **No leak exists.** `juce::MemoryMappedFile` closes its fd immediately after mmap
  (`juce_SharedCode_posix.h:587-604`); sockets are RAII; Tracktion's graph workers are joined
  in `clearThreads` (`tracktion_LockFreeMultiThreadedNodePlayer.cpp:233-237`) and
  `EditPlaybackContext::deallocate` calls `setNumThreads(0)` (`:643`); `HostedAudioDriver`
  joins its `std::thread` in `stop()` and the destructor calls `stop()`.
- JUCE uses `poll()` on POSIX (`juce_Socket.cpp:371-385`), so there is no `FD_SETSIZE` path.

For completeness: on Windows I measured peak `HandleCount` at 222 for the OSCQuery cases
alone, 353 for the audio cases, 431 for everything, and traced the audio cases climbing
136 → 356 without falling back. That looks alarming and is what sold me on it. **Windows
handles are not POSIX descriptors** — they count threads, events and mutants too — so the
climb is most likely threads and kernel objects, not files. `ulimit -n` costs ten seconds and
is in the runbook, but do not let this hypothesis win on tidiness.

### 3b. Tracktion leaving the main thread at Mach realtime priority — REFUTED

Worth writing down because it is a genuine upstream bug and it looked like the perfect answer
to "why the audio tests". In all five thread pools
(`tracktion_NodePlayerThreadPools.cpp:59, 194, 304, 460, 567`):

```cpp
for (size_t i = 0; i < numThreads; ++i)
{
    threads.emplace_back ([this] { runThread(); });
    setThreadPriority (threads.back(), 10);
    tryToUpgradeCurrentThreadToRealtime (rtOpts);   // <- the CALLER, not threads.back()
}
```

That upgrades whoever called `createThreads` — the thread that built the Edit — to a Mach
`THREAD_TIME_CONSTRAINT` thread on macOS, and never restores it. Real, and worth reporting
upstream one day.

**It cannot be firing here.** `EditPlaybackContext` sets
`setNumThreads (getNumberOfCPUsToUseForAudio() - 1)`
(`tracktion_EditPlaybackContext.cpp:1113`), and Go.dot's `EngineBehaviour` returns **1**
(`src/wfg/engine/audio/AudioHost.cpp:112`). So `numThreads == 0`, the loop body never
executes, and the call is never made. Verified in three steps.

---

## 4. Already tried, and it did not fix it

Every audio rig used to construct its own `juce::ScopedJuceInitialiser_GUI`, so
`initialiseJuce_GUI` / `shutdownJuce_GUI` ran dozens of times per process. That was hoisted to
one initialiser for the life of `main()` (`tests/TestMain.cpp`, commit `25e1d8a`).

It **changed the symptom** — line 591 appeared only afterwards — and did not fix it. Keep it;
it is right on its own merits. But note the constraint it imposes on any hypothesis: a
mechanism that the hoist *reduced* cannot be the cause, or you are arguing that less of it
made things worse.

---

## 5. The runbook

### Step 0 — the move that may not need the Mac at all

The whole diagnosis turns on one question: **was `rig.port()` zero?** Two ways to answer it
without leaving your desk:

```bash
# (a) the macOS job builds Debug, so JUCE's assertion output is live and ctest captures stderr
gh run view <failed-macos-run-id> --log-failed | grep -nE "juce_Socket\.cpp|Assertion failure"
```

A `JUCE Assertion failure in ... juce_Socket.cpp:562` line is near-proof: that `jassert` fires
only for a port outside 1..65535.

```
# (b) make the test say it out loud — add to tests/OscQueryTests.cpp Rig, around line 344:
        Rig() { started = server.start (0, nameSpace); }
        ...
# and in the two failing cases, after REQUIRE (rig.started):
        REQUIRE (rig.port() > 0);
```

Push that and let CI answer. A red `rig.port() > 0` confirms §2 outright, and it is a check
the rig should have had anyway.

### Step 1 — the fix, which is one line in *our* code

Do not patch the submodule; `check-pins.py` will fail the pins gate. Fix the reader instead,
in `src/wfg/engine/oscquery/OscQueryServer.cpp:316`:

```cpp
for (int i = 0; i < 2000 && ! (impl->server.isConnected && impl->server.port > 0); ++i)
    juce::Thread::sleep (5);
```

and make the failure check at `:319` test both, so a still-zero port at timeout is a failed
start rather than a server nobody can reach. Fix the comment above it too — it currently
states the thing that is not true.

This is correct regardless of whether it is *the* cause: a server whose port is 0 has not
started, and saying it has is a lie the rest of the process then acts on.

### Step 2 — build and reproduce on the Mac

```bash
brew install ccache ninja
python3 -m pip install lxml        # hard configure-time requirement
git submodule update --init --recursive ThirdParty/juce_simpleweb

cmake --preset ci-macos
cmake --build --preset ci-macos-debug
B=$PWD/build/ci-macos/tests/Debug/wfg_tests
```

Reproduce before fixing, so you know the fix did something:

```bash
for i in $(seq 20); do "$B" --wfg-locale=C >/dev/null 2>&1 || echo "FAIL run $i"; done
```

Your Mac mini is idle and faster than a 3-core CI runner, so it may not reproduce. If it does
not, load it (`for i in 1 2 3 4 5 6 7 8; do yes > /dev/null & done`) and try again.

### Step 3 — force the race, which is the cleanest confirmation

In a **scratch worktree** (this dirties a pinned submodule — revert it), insert
`juce::Thread::sleep (200);` between `SimpleWebSocketServer.cpp:251` and the
`ioService->run()` at `:258`. That holds the window open on every rig, on **every platform**.
Then:

```bash
"$B" --wfg-locale=C --source-file='*OscQueryTests.cpp'
```

If that reproduces CI's signature, §2 is proven and it can be proven on Windows too. Then
apply the Step 1 fix and confirm the forced-race build goes green with the sleep still in.

### Step 4 — the isolation split, if you need it

```bash
"$B" --wfg-locale=C --source-file='*OscQueryTests.cpp'                        # alone
"$B" --wfg-locale=C --source-file='*AudioTests.cpp,*OscQueryTests.cpp'        # 51 cases
```

Alone-fails → the audio tests are irrelevant and §2 stands by itself. Alone-passes but the
pair fails → the audio tests are a necessary amplifier, and the open question at the end of
§2 becomes worth answering.

Run order is settled: doctest 2.4.11 defaults to `--order-by=file` and sorts by the `__FILE__`
string then line (`doctest.h:6662, 6879-6903, 4365-4375`). `AudioTests.cpp` sorts first, so
all the audio cases run before everything else. That is *not* link order — do not assume it.

---

## 6. Traps that will cost you time

- **Do not use `--test-case=`.** doctest splits filters on commas (`doctest.h:6622`) and many
  Go.dot test names contain commas, so `--test-case='oscquery: four questions, four different
  answers'` matches nothing. Use `--source-file='<glob>'` with `--first=N --last=N`.
- **Quote every glob.** The macOS shell is zsh, where an unmatched glob is a hard error rather
  than a pass-through.
- **Test indices are not portable** from the Windows box; re-derive with `--list-test-cases`.
- **One green run is not a result.** This is a race. Twenty iterations minimum, under load.
- **Do not "fix" it with a retry, a longer `waitUntil`, or a sleep after `server.start()`.**
  Every mechanism still standing is a real defect. A retry hides it until a show night.
- **Do not build under Rosetta to "test the memory model".** A green x86-64 run would only
  prove the timing changed, and it costs a full rebuild.
- **Do not run under ThreadSanitizer expecting a verdict.** It will report the
  `port`/`isConnected` race on every run whether or not that race is the failure — the race is
  already established by reading `SimpleWebSocketServer.h:34,37` — and bury it in Tracktion
  noise.

---

## 7. If §2 is refuted

Then the port was fine and something else stopped the connect. In order:

1. **`http->config.timeout_request = 1;`** (`SimpleWebSocketServer.cpp:240`) — a **one-second**
   deadline on a single-threaded reactor. Raise it to 30, rebuild, re-run. If failures vanish
   while the port stays correct, the io thread was being starved past its request deadline and
   the audio tests are the starvation.
2. **The force-kill path in `stopInternal`** (`SimpleWebSocketServer.cpp:160-167`) ends in
   `stopThread(1000)`; if the io thread does not exit within a second, JUCE logs
   `!! killing thread by force !!` and calls `pthread_cancel`, leaving a listening socket open
   and `serverLock` permanently held. Grep the failing CI log for that literal string — if it
   is there, a server thread was cancelled mid-run.
3. **`ulimit -n`** (see §3a) — ten seconds, only to close it out.

---

## 8. What "fixed" looks like

- `ctest --preset ci-macos` green on the Mac, twenty runs in a row, under load.
- Green on Windows and Linux CI too — do not fix macOS by breaking the other two.
- The fix addresses the cause, in `src/`, not in a pinned submodule and not in a timeout.
- **Watch `tests/SimpleWebToolchainTests.cpp`.** It sorts *after* `OscQueryTests.cpp` and does
  the same handshake with only three rigs, so it is the control group. If it starts failing
  after your change you moved the race rather than closed it.
- **Write down what you find.** A JUCE/Tracktion lifecycle fact belongs in §11.8 of
  `docs/godot-namespace-draft-0.1.md` with the other Phase 2 measurements. An upstream defect
  belongs in `docs/spikes/` written as a report — `upstream-node-id-collision.md` is the model.
  The `tryToUpgradeCurrentThreadToRealtime` bug in §3b deserves that treatment whether or not
  it is this failure.

---

## 9. Repo conventions — not negotiable, and CI enforces most of them

- **Commit straight to `main`.** The author has said so explicitly; no branches.
- **Never edit `CLAUDE.md` or PRD §4.** `CLAUDE.md` is PRD §4 reproduced byte-exactly and a CI
  gate compares them. If they disagree the PRD wins and `CLAUDE.md` is fixed *by copying*.
- **GPL-3 header and `SPDX-License-Identifier: GPL-3.0-or-later`** on every source file.
- **The comment gate.** `ctest -R source.comments` fails on `/*` inside a block comment. It
  caught me three times. Reword the prose; never escape the delimiter.
- **Both locales.** Every serialisation test runs under `C` and `fr_FR`. Do not weaken it.
- **Line endings** are pinned per extension in `.gitattributes`; a new extension needs a rule.
- **Never patch a submodule on `main`** — `check-pins.py` fails the pins gate.
- **Commit messages** are prose explaining *why*, in the style of the existing log. Read
  `git log` first. End with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.
- **Author decisions are surfaced as questions**, not resolved by picking the reasonable-looking
  option.

---

## 10. Context you may want

**The new code** is in `src/wfg/engine/audio/` — `AudioHost` (Tracktion behind a pimpl,
vendor-free header), `CueOutputPlugin`, `CueMatrix` (JUCE only), `HostedAudioDriver`,
`AudioCommands` — and `src/wfg/engine/rt/` (the audio-thread allocation counter). All the audio
tests are in `tests/AudioTests.cpp`, ~49 cases.

**A trap already documented**, in case you meet it: hosting Tracktion sets flush-to-zero
process-wide, which broke three unrelated number-formatting tests depending on run order. It is
scoped with `juce::ScopedNoDenormals` in `AudioHost` and has a regression case. If arithmetic
misbehaves in a test with no business touching audio, that is why.

**`ad7e082` has never been compiled by Clang.** It replaces the global `operator new` and there
is no Clang on the Windows box. GCC compiles it clean on Linux CI, so it should be fine — but
if the Mac build fails *there* rather than in the tests, that is a separate and much simpler
problem.

**Recent commits**, newest first:

```
25e1d8a Three GCC errors and a macOS flake, all of them Phase 2's doing
ad7e082 posix_memalign by its own header, for the platforms this box cannot compile
0a51b2e A .txt rule, because the warning was the point of the last four
f9d7ffb The second net: a preset that will not configure here, and says why
b8a6d96 The lipogram, instrumented: zero for us, measured for Tracktion
2fb60dc What 2.1 measured, written down where the next three PRs will look
5ee3d9d The graph coming into being is an event, not a variable being set
22e3b11 --hosted: the same show, with the graph under the clock
29b1688 M1: a cue lands where it was sent, and keeps landing there
38bf76c Even asked properly, the question was put to the wrong graph
```
