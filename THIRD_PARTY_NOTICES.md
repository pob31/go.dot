# Third-Party Notices

Go.dot incorporates the third-party libraries below. Their licences and
copyright notices are recorded here, once, as fact.

This file records notice obligations, per dependency. Go.dot's own licence, and
how it relates to its dependencies', is discussed once in
[`README.md`](README.md#a-note-on-juce) and nowhere else in this tree.

Two submodules supply everything below. Nothing else is vendored by Go.dot
itself, and no dependency here is fetched at configure time — the pins are in
`.gitmodules` and enforced by `scripts/check-pins.py`.

---

## JUCE Framework

- **Website**: https://juce.com
- **Version**: 8.0.6+19 (`develop`), commit `19edd538429c93d277bf95b55aaa7e3eb545f951`
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
- **Version**: v3.2.0, commit `0a5f4e6a5f53d09c89b414a44386a12df7fa1ec6`
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
