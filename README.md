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

---

## Status: Phase 1 in progress

Phase 0 — *"a repo that builds on three platforms"* — is complete, and the seven
Tracktion Engine validation spikes have been run (`docs/spikes/`; all pass). Phase 1, the
headless engine that loads a show document and exposes it over OSCQuery, has started with
its two documents:

- **[`docs/godot-namespace-draft-0.1.md`](docs/godot-namespace-draft-0.1.md)** — the
  *shape* of the `/godot` namespace and the show document: how a node is addressed, what
  metadata it carries, how a mutation happens and how it is recorded. A living document,
  because Go.dot is not a port of something that already works and there is no finished
  parameter list to transcribe.
- **[`docs/parameters/godot-parameters.csv`](docs/parameters/godot-parameters.csv)** —
  *what* exists, added to as each phase lands. One table generating four surfaces: the
  document schema, the parameter tree, the RELAX NG schema and the OSCQuery reply. WFS-DIY
  keeps three of those independently and reconciles them with a runtime drift auditor;
  starting collapsed is cheaper than collapsing later.
- **[`docs/godot-reuse-map-0.1.md`](docs/godot-reuse-map-0.1.md)** — what WFS-DIY,
  spatcore and juce_simpleweb already provide, per phase, and what stops each piece being
  used as-is.

**What exists**

- CMake build wired to JUCE and Tracktion Engine as pinned submodules, with the
  vendor sources compiled exactly once into `wfg_thirdparty`.
- `wfg`, a console binary that boots and exits: `wfg --version` prints the JUCE
  and TE versions it actually linked, `wfg selftest` stands the JUCE message
  thread up headless and exits 0.
- `wfg_tests`, a doctest suite that asserts the toolchain facts a green compile
  does not prove — the JUCE pin at *runtime*, the module configuration actually
  reaching our targets, and every case run twice, under `C` and under `fr_FR`.
- GitHub Actions CI for Linux, macOS and Windows, plus a pin gate and an
  isolated job that builds the spikes.
- `spikes/`, for the seven Tracktion Engine validation programs of PRD §6.1.
  They are throwaway by construction: they may link `wfg::thirdparty` and never
  `wfg::engine`, so there is nothing in them that *could* migrate into `src/`.

**What does not exist yet** — no engine subsystems at all: no parameter tree, no
OSCQuery server, no cue model, no tick clock, no document format, no UI, no
plugin hosting, no video. Phase 1 builds the first five, in this order: the engine
skeleton (commands, tick-indexed event log, replay), the document, the tick clock, the
parameter tree with mounted namespaces as stubs, the cue list with one standby pointer,
and last the OSCQuery server on `juce_simpleweb`. Two further submodules arrive with it —
`ThirdParty/juce_simpleweb` and `ThirdParty/spatcore` — the first for the HTTP+WebSocket
transport, the second consumed at source level for its real-time helpers.

**Open questions, deliberately unanswered anywhere in this tree**

The devplan lists these under Phase 0's *"Needs from the author"*. A default
picked here would be an answer to a question that has not been asked, so there
is no `option()`, no cache variable, no preset value and no `constexpr` for
either of them anywhere in the build:

1. **Default fixed track count.** Spike #4 — graph stability under sustained
   launching, the devplan's first priority — cannot run without one. It takes it
   as `--tracks=N` on the command line.
2. **Target sample rates and buffer sizes.** Same treatment (`--sample-rate=N
   --buffer=N`). PRD §3.4's "96 kHz / 64 frames" is an arithmetic illustration,
   not a specification.

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
3. Python 3 on `PATH`, for `scripts/check-pins.py`.

**macOS**

1. [Xcode](https://apps.apple.com/app/xcode/id497799835) from the App Store, or
   the command line tools: `xcode-select --install`. Xcode 15 is the floor.
2. `brew install cmake ninja ccache`.
3. **macOS 13.3 is the deployment target**, and that number is not arbitrary: it
   is where Apple's libc++ made `std::to_chars` available for floating-point
   types, which is what Go.dot writes every number with. JUCE's own writer loses
   46% of doubles to a save-and-load round trip (measured; the table is in
   `src/wfg/engine/osc/OscValue.cpp`), and a show file whose numbers change when
   you reopen it is not a show file.

**Linux**

1. GCC 11 or newer (or Clang 14+), CMake 3.22 or newer.
2. `bash scripts/install-linux-deps.sh` — that script *is* the package list, and
   CI runs the same file, so it cannot rot.

### Step by step

**1. Clone with submodules**

```bash
git clone --recurse-submodules https://github.com/pob31/go.dot.git
cd go.dot
./scripts/bootstrap.sh          # scripts\bootstrap.ps1 on Windows
```

`bootstrap` is idempotent — re-run it any time. It initialises the two
submodules, checks the pins, and disarms Tracktion Engine's nested SSH JUCE
submodule so the recursive command below cannot bite you later.

> **Important:** do **not** use `--recursive` on the submodule update, and do not
> use `--depth 1`.
>
> `git submodule update --init --recursive` descends into
> `tracktion_engine/modules/juce`, whose URL in TE's own `.gitmodules` is
> `git@github.com:juce-framework/JUCE.git` — SSH. Without a registered SSH key it
> fails with `Permission denied (publickey)` three levels down, in a message that
> names neither Go.dot nor Tracktion Engine. Every CI runner is in exactly that
> position. Go.dot pins JUCE itself, so TE's vendored copy is redundant: our
> CMake adds `ThirdParty/tracktion_engine/modules` and never TE's root, and that
> directory can stay empty forever.
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

The suite runs four tests: the unit binary twice (once under `C`, once under
`fr_FR`), and the product binary twice (`wfg --version`, `wfg selftest`).

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

Build presets append `-debug` / `-release` (`dev-debug`, `ci-linux-release`, …).

### Targets

| Target | Kind | What it is |
|---|---|---|
| `wfg::deps` | INTERFACE | Include paths, the JUCE/TE macro environment, C++20, platform flags |
| `wfg::warnings` | INTERFACE | The warning policy. Linked to **our** targets only, never to vendor code |
| `wfg::thirdparty` | STATIC | The one place JUCE and Tracktion Engine module sources compile |
| `wfg::engine` | STATIC | The engine library. Its public headers name no JUCE or TE type |
| `wfg` | executable | The product binary (PRD §7's binary name) |
| `wfg_tests` | executable | doctest runner, registered with CTest under both locales |
| `spike01…07_*` | executables | PRD §6.1 validation programs, behind `WFG_BUILD_SPIKES` |

---

## Dependency pins

| Dependency | Version | Commit |
|---|---|---|
| JUCE | 8.0.13+7 (on `develop`) | `19edd538429c93d277bf95b55aaa7e3eb545f951` |
| Tracktion Engine | develop (3.5.0) | `0a5f4e6a5f53d09c89b414a44386a12df7fa1ec6` |

The load-bearing fact: **Tracktion Engine develop (3.5.0)'s own `modules/juce` gitlink is
byte-for-byte our JUCE pin.** We are not guessing at a compatible JUCE — we are
using the one TE was tested against, while pinning it ourselves so the URL is
HTTPS and the build never enters TE's root `CMakeLists.txt`. Keeping that
equality true is the whole job of `scripts/check-pins.py`.

### Bumping a pin

1. Move `ThirdParty/tracktion_engine` to the new tag.
2. Read TE's new vendored JUCE SHA:
   `git -C ThirdParty/tracktion_engine ls-tree HEAD modules/juce`.
3. Move `ThirdParty/JUCE` to **that exact SHA**. Not to the tip of `develop`, not
   to the nearest tag.
4. Run `python3 scripts/check-pins.py`, then the presets locally on at least one
   platform.
5. **Commit both gitlinks in one commit**, so a bisect can never land on a
   skewed pair.

Never `git submodule update --remote` — it moves a pin to a branch tip behind
your back, which is the one thing a pin exists to prevent. Never `--recursive`,
anywhere. A deliberate JUCE bump ahead of TE is possible, but the person doing it
says so out loud: `check-pins.py --allow-skew`.

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
cmake/               guards, options, third-party wiring
docs/                the PRD and the development plan — the spec
scripts/             bootstrap, the Linux package list, the pin gate
src/                 wfg_engine (the library) and wfg (the binary)
tests/               the doctest suite and every add_test() in the project
spikes/              throwaway PRD §6.1 validation programs
ThirdParty/          JUCE and tracktion_engine submodules
```

Two directories that do **not** exist here, and will not:

- The Phase 7 tablet client goes in `clients/tablet/` with its own toolchain and
  is never an `add_subdirectory` of this build. It is a client over OSCQuery like
  any other, and a web toolchain inside a CMake tree helps nobody.
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
  Decision points under *"Needs from the author"* are his — surface them as
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
