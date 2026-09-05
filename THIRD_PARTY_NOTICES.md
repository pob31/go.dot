# Third-Party Notices

Go.dot incorporates the third-party libraries below. Their licences and
copyright notices are recorded here, once, as fact.

This file records notice obligations, per dependency. Go.dot's own licence, and
how it relates to its dependencies', is discussed once in
[`README.md`](README.md#a-note-on-juce) and nowhere else in this tree.

Four submodules supply everything below, plus one nested inside another
(`asio`, under `juce_simpleweb`). Nothing else is vendored by Go.dot itself, and
no dependency here is fetched at configure time — the pins are in `.gitmodules`
and enforced by `scripts/check-pins.py`.

---

## JUCE Framework

- **Website**: https://juce.com
- **Version**: 8.0.13+7 (`develop`), commit `37c894f83d379179b2070d437ccd0f1cd9af9576`
- **Licence**: AGPLv3 or commercial JUCE Licence
- **Copyright**: Raw Material Software Limited

The JUCE modules are dual-licensed under the
[AGPLv3](https://www.gnu.org/licenses/agpl-3.0.en.html) and the commercial
[JUCE 8 End User Licence Agreement](https://juce.com/legal/juce-8-licence/).
Go.dot uses JUCE under the AGPLv3 path. Full text:
`ThirdParty/JUCE/LICENSE.md`.

Go.dot links `juce_core`, `juce_events`, `juce_data_structures`,
`juce_audio_basics`, `juce_audio_formats`, `juce_audio_devices`,
`juce_audio_processors`, `juce_audio_utils`, `juce_dsp` and `juce_osc`.
`juce_graphics`, `juce_gui_basics` and `juce_gui_extra` arrive transitively
through `juce_audio_processors` — an unbreakable chain in JUCE 8, which is why
this headless Phase 0 binary is nevertheless GUI-linked and why the Linux
package list carries X11, freetype and fontconfig.

JUCE bundles further dependencies with their own licences; the two that reach
every Go.dot binary through `juce_graphics` are listed separately below. See
`ThirdParty/JUCE/LICENSE.md` for the complete list.

---

## Tracktion Engine

- **Website**: https://github.com/Tracktion/tracktion_engine
- **Version**: develop (3.5.0), commit `b88a6ee51913668cb53e911e030ab736b13342cf`
- **Licence**: GPLv3 or commercial Tracktion licence
- **Copyright**: Tracktion Corporation

Go.dot uses Tracktion Engine under the GPLv3 path, and links the
`tracktion_core`, `tracktion_engine` and `tracktion_graph` modules.

Tracktion Engine vendors several libraries of its own inside
`modules/3rd_party/` and `modules/tracktion_engine/3rd_party/`. The ones that
actually compile into a Go.dot binary, or that Go.dot uses directly, are listed
below. The rest — airwindows, crill, expected, magic_enum, nanorange, rigtorp —
are permissively licensed and reach us only as headers Tracktion Engine
includes; see the files in those directories for their notices.

---

## doctest

- **Website**: https://github.com/doctest/doctest
- **Version**: 2.4.11 (vendored by Tracktion Engine at
  `modules/3rd_party/doctest/`)
- **Licence**: MIT
- **Copyright**: (c) 2016-2023 Viktor Kirilov

Go.dot's unit-test suite (`wfg_tests`) uses doctest through Tracktion Engine's
own wrapper header, `<3rd_party/doctest/tracktion_doctest.hpp>` — never
`doctest.h` directly, so TE's warning suppression comes with it. Using the
vendored copy means Go.dot needs no additional test-framework submodule.

---

## libsamplerate

- **Website**: https://github.com/libsndfile/libsamplerate
- **Licence**: BSD-2-Clause
- **Copyright**: (c) 2002-2016 Erik de Castro Lopo <erikd@mega-nerd.com>

Vendored by Tracktion Engine at `modules/3rd_party/libsamplerate/` and compiled
**unconditionally** into `tracktion_engine_playback.cpp`. That is the fact
behind a build-system decision worth recording: varispeed works in Go.dot with
no build flag at all, which is part of why none of the
`TRACKTION_ENABLE_TIMESTRETCH_*` options are set.

Full licence text:
https://github.com/libsndfile/libsamplerate/blob/master/COPYING

---

## SoundTouch

- **Website**: https://www.surina.net/soundtouch
- **Licence**: LGPL-2.1-or-later
- **Copyright**: (c) Olli Parviainen

Vendored by Tracktion Engine at
`modules/tracktion_engine/3rd_party/soundtouch/`.

Measured, and the reason this entry exists at all: three SoundTouch translation
units — `BPMDetect.cpp`, `PeakFinder.cpp` and `FIFOSampleBuffer.cpp` — are
included **outside** the `#if TRACKTION_ENABLE_TIMESTRETCH_SOUNDTOUCH` guard in
`tracktion_engine_timestretch.cpp`, and therefore compile into every binary
regardless of the time-stretch flags. Go.dot sets none of those flags; SoundTouch
code is in the binary anyway.

LGPL-2.1-**or-later** upgrades to LGPL-3, which is compatible with GPL-3.0, so
this is a notice obligation and not a conflict.

---

## rpmalloc

- **Website**: https://github.com/mjansson/rpmalloc
- **Licence**: Public Domain (also available under MIT)
- **Copyright**: (c) 2016 Mattias Jansson

Vendored by Tracktion Engine at `modules/3rd_party/rpmalloc/`, which uses it for
its real-time allocator.

---

## choc

- **Website**: https://github.com/Tracktion/choc
- **Version**: 1.0.0 (vendored by Tracktion Engine at `modules/3rd_party/choc/`)
- **Licence**: ISC
- **Copyright**: (c) Julian Storer / Tracktion

Header-only. Reaches Go.dot through Tracktion Engine, including via the doctest
wrapper header the test suite uses.

---

## HarfBuzz

- **Website**: https://harfbuzz.github.io
- **Licence**: MIT (the "Old MIT" HarfBuzz licence)
- **Copyright**: (c) HarfBuzz authors and contributors

Bundled with JUCE and compiled into `juce_graphics`
(`juce_graphics_Harfbuzz.cpp`). It is in every Go.dot binary because
`juce_graphics` is, even though Phase 0 draws nothing. Full text in
`ThirdParty/JUCE/LICENSE.md`.

---

## SheenBidi

- **Website**: https://github.com/Tehreer/SheenBidi
- **Licence**: Apache-2.0
- **Copyright**: (c) Muhammad Tayyab Akram

Bundled with JUCE and compiled into `juce_graphics`
(`juce_graphics_Sheenbidi.c`), on the same terms as HarfBuzz above. Apache-2.0
is compatible with GPL-3.0. Full text in `ThirdParty/JUCE/LICENSE.md`.

---

## juce_simpleweb

- **Website**: https://github.com/benkuper/juce_simpleweb
  (Go.dot pins the fork https://github.com/pob31/juce_simpleweb)
- **Version**: commit `b953ada40073d8e7338fbc13aa6bccf3fe70087d`
- **Licence**: GPLv3
- **Copyright**: Ben Kuper

A JUCE module wrapping Simple-Web-Server, serving HTTP and WebSocket on a single
port. That is not a convenience: the OSCQuery specification requires both on the
same port, and it is the reason this dependency exists rather than
`juce::StreamingSocket`.

Go.dot compiles it with `SIMPLEWEB_SECURE_SUPPORTED=0` — plain HTTP and WS, no
TLS — and clears the module's declared OpenSSL link libraries outright. The
`deps.no-openssl` test asserts that on the shipped binary, so this paragraph is
checked rather than merely asserted. See `cmake/WfgThirdParty.cmake` section 2b.

The pinned fork is upstream master plus two changes the author needed and has
offered upstream: a build-time TLS-off guard, and a Windows fix. Full text:
`ThirdParty/juce_simpleweb/LICENSE`.

---

## Simple-Web-Server

- **Website**: https://gitlab.com/eidheim/Simple-Web-Server
- **Version**: vendored inside `ThirdParty/juce_simpleweb/` (`webserver/`, `websocket/`,
  `common/`)
- **Licence**: MIT
- **Copyright**: Ole Christian Eidheim

The HTTP and WebSocket server implementation that juce_simpleweb wraps. Reaches
a Go.dot binary as compiled code — `SimpleWeb::Server<HTTP>` is what answers
every OSCQuery request.

**The vendored copy carries no licence header or notice file of its own.** The
attribution above is from the upstream project, which the code is otherwise
identifiable as (the `SimpleWeb` namespace, the `SIMPLE_WEB_SERVER_*_HPP`
include guards). That is a notice-obligation gap in the module rather than in
Go.dot, and it is recorded here rather than quietly papered over: the fork is
`pob31/juce_simpleweb`, so it is fixable at the source by adding upstream's
LICENSE file to the vendored directory.

`common/crypto.hpp` is part of this copy and calls OpenSSL's `SHA1()` directly.
It is unreachable in a Go.dot build — the WebSocket handshake goes through
`WSCrypto::calcSha1` instead (`websocket/server_ws.hpp:565`), which is the
self-contained implementation listed below — and `deps.no-openssl` is what keeps
that true rather than assumed.

---

## asio (standalone)

- **Website**: https://think-async.com/Asio/
- **Version**: commit `6caa38aa03246140d745f36207892713895d245e`, as
  `ThirdParty/juce_simpleweb/asio` (the fork `benkuper/asio`)
- **Licence**: Boost Software License 1.0
- **Copyright**: Christopher M. Kohlhoff

The networking layer under Simple-Web-Server, used **standalone** — without
Boost. This is the one nested submodule Go.dot deliberately populates: it is
required to compile juce_simpleweb at all, its URL is HTTPS (unlike Tracktion
Engine's vendored JUCE), and `scripts/check-pins.py` check (f) asserts it is
present.

The licence is stated in the banner of every header — e.g.
`ThirdParty/juce_simpleweb/asio/basic_socket.hpp:1-9` — which points at an
accompanying `LICENSE_1_0.txt`. **That file is not in this checkout**: the fork
vendors asio's headers without its repository root. The canonical text is at
https://www.boost.org/LICENSE_1_0.txt and the banners themselves satisfy BSL-1.0
clause 1, which requires the notice to travel with the source.

---

## SHA-1 implementation (in juce_simpleweb's WSCrypto)

- **Licence**: BSD 3-Clause
- **Copyright**: Micael Hildenborg

`ThirdParty/juce_simpleweb/common/WSCrypto.h` and `.cpp`, whose notice is
reproduced in full at the top of the header.

The WebSocket handshake hashes the client key with SHA-1, because RFC 6455
requires exactly that: it is a protocol constant, not a security choice, and no
part of Go.dot uses SHA-1 for anything else. This is the implementation the
handshake actually calls (`WSCrypto::calcSha1`), which is how the build manages
to need no OpenSSL.

---

## spatcore

- **Website**: https://github.com/pob31/spatcore
- **Version**: commit `7e1a8adf063cffcd8f33c7b2be7ad515693e5dd3`
- **Licence**: GPLv3
- **Copyright**: Pierre-Olivier Boulant

The author's shared control-plane and real-time support code, also used by
WFS-DIY, XOA and Tight-WFS. Consumed at **source level** — there is no
`add_subdirectory`, because spatcore's own CMake targets call
`juce_add_modules()` and would compile JUCE a second time in this build tree.

Phase 1 compiles exactly one header from it, `rt/RtThreadPriority.h`, which is
JUCE-free. Phase 2 is expected to add `rt/RtSnapshot.h` (juce_core only). Most of
`control/` requires JUCE 9 and is unreachable from this build, which is why
Go.dot has an OSC codec of its own. Full text: `ThirdParty/spatcore/LICENSE`.

---

## Not present, and why it is worth saying

- **RubberBand** is *not* a dependency. Enabling it would mean a licence decision
  (PRD §3.25's "licence permitting") plus a fourth submodule that hard-`#error`s
  on a clean clone. All four `TRACKTION_ENABLE_TIMESTRETCH_*` flags are left at
  0; Tracktion Engine degrades cleanly with them off.
- **libcurl**, **WebKitGTK**, **JACK** and the **LADSPA SDK** are kept out of the
  build by `JUCE_USE_CURL=0`, `JUCE_WEB_BROWSER=0`, `JUCE_JACK=0` and
  `JUCE_PLUGINHOST_LADSPA=0`. Each is a package the Linux dependency list does
  not have to carry; `scripts/install-linux-deps.sh` records the pairing.
- **The LV2 SDK** (lilv, serd, sord, sratom) *is* vendored by JUCE and attached
  automatically. When Phase 9 turns plugin hosting on, LV2 costs one compile
  definition and zero system packages on all three platforms.
- **OpenSSL** is *not* a dependency, and keeping it that way took deliberate
  work. `juce_simpleweb` declares `libssl` and `libcrypto` as link libraries on
  all three platforms — and in two different spellings, bare on Linux and
  `lib`-prefixed elsewhere — even though Go.dot compiles it with TLS off and
  calls no TLS code. `cmake/WfgThirdParty.cmake` clears that property outright
  rather than filtering it by name, and the `deps.no-openssl` ctest inspects the
  built binary's actual dynamic dependencies on every platform. A Go.dot that
  linked OpenSSL would refuse to start on a machine without the runtime, in
  service of a feature it does not have.
