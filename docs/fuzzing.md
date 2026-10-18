# Fuzzing

The unit suite pins the malformed inputs someone thought of. Fuzzing looks for the
ones nobody did, in the modules that read bytes Go.dot did not write: a file from
another application, a datagram from the network. A crash or a hang there is a
bug on the path that must never block (PRD §4, law 1) or in a show file someone
trusts (law 3).

## What is fuzzed

| Target | Module | Contract it checks |
|---|---|---|
| `fuzz_osc` | `OscCodec` | never crashes, never reads outside the buffer; a refusal always carries `reason` and `error`; what decodes also encodes, and decodes again to the same packet (a packet holding a NaN or infinity is exempt - the decoder delivers it, the entry points downstream refuse it) |
| `fuzz_bplist` | `Bplist` (`parse`, `Archive`) | never reads outside the file; a refusal always says why; every reference in a parsed table points inside it; a parsed archive is walked as the QLab import walks it, including archives held as data |

| `fuzz_qlab` | `QlabReader`, `QlabWalk` | a workspace's bytes never read past the file; a refusal always says why; what reads is walked three ways - every list; every cue given a channel count, a picture size and a font; and with zero and negative sizes |
| `fuzz_als` | `AlsReader`, `AlsWalk` | the set's XML never read past its text; a refusal always says why; what reads is walked whole, scene by scene, with scenes out of range, and every clip's warp asked for hostile beats and a tempo of nought |

The first two are standard-library only. The QLab and Ableton targets need
`juce_core` (they take a `juce::File` or an `XmlElement`), which the build compiles
as one translation unit, uninstrumented, from `ThirdParty/JUCE` - checked out
shallow, it is the only submodule needed. They are skipped with a message when it
is absent, and for now are Linux-only. They link a copy of one two-line function
from `ImportCommon.cpp` (`tests/fuzz/FuzzImportShim.cpp`), which cannot be linked
whole: change the two together. The importers above them (`QlabImport`,
`AlsImport`) reach the document model and are not fuzzed. Not fuzzed, on purpose: the engine, the cues and the GO logic (ordering
and state, which the suite covers), the audio thread (rtsan), and hardware paths.

## Running

```
cmake -S tests/fuzz -B build/fuzz && cmake --build build/fuzz
ctest --test-dir build/fuzz                          # 20000 mutations per target
git submodule update --init --depth 1 ThirdParty/JUCE   # first, for fuzz_qlab and fuzz_als
build/fuzz/fuzz_osc --mutate 1000000 42              # longer, seed 42
build/fuzz/fuzz_bplist crash-input                   # replay a failing input
```

Or `-DWFG_BUILD_FUZZ=ON` on the main configure. GCC or Clang; ASan and UBSan are
always on. The default build uses the built-in mutator (`FuzzDriver.cpp`), which is
deterministic: the same seed number is the same run. For coverage-guided runs with
Clang and libFuzzer:

```
cmake -S tests/fuzz -B build/fuzz-lf -DCMAKE_CXX_COMPILER=clang++ -DWFG_FUZZ_ENGINE=libfuzzer
build/fuzz/fuzz_osc --write-seeds corpus     # the driver build writes the valid seeds
build/fuzz-lf/fuzz_osc corpus -max_total_time=600
```

## When one fails

The failing input is left as `crash-input` in the working directory. Reproduce it,
fix the module, and add the case to that module's own tests (`BplistTests.cpp`,
`OscCodecTests.cpp`) - the fuzz target finds it, the unit test keeps it found.

## Found so far

Each is pinned by a case in the module's own tests.

- `Archive::className` indexed the object table with `none` when a class's
  `$classname` was itself a UID resolving to `$null` or past the archive: an
  out-of-bounds read on a malformed QLab workspace. Fixed; pinned in
  `BplistTests.cpp`.
- The Ableton set reader and walker, on their first run:
  - **A loop end of 1e308 beats made `AlsWalk`'s curve grid run until memory did** (and a
    zero step would have run for ever). The grid is bounded at 100000 points and skipped
    for a span or step that is not a number. `AlsWalkTests.cpp`.
  - **Signed overflow in `AlsReader`**: an attribute past an int's range was added to
    (`channel + 1`). `integer()` now saturates and reads a NaN as its fallback, and a
    MIDI channel past 15 - Live counts 0 to 15 - is no mapping. `AlsReaderTests.cpp`.
  - **A read past the end of the text inside JUCE's `XmlDocument`** when a set stops in
    the middle of an `&#x` entity: its `readEntity` steps over the end it just found.
    What it then reads is the allocation's leftover bytes, which is why the input fails
    only after other inputs have run. `readSetXml` refuses a text that does not end at a
    tag, in words ("it ends before its last tag"), which is also what a cut-short file is.
    `AlsReaderTests.cpp`.
  - **`flagResting` read a slot that was not there** when a return carries a track's Id
    (the lookup by Id finds the return, which has no slots). `AlsWalkTests.cpp`.
