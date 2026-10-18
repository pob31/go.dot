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

Both modules are standard-library only, so a target compiles its own sources and
needs neither JUCE nor Tracktion. A module that gains a JUCE dependency leaves
that list. Not fuzzed, on purpose: the engine, the cues and the GO logic (ordering
and state, which the suite covers), the audio thread (rtsan), and hardware paths.

## Running

```
cmake -S tests/fuzz -B build/fuzz && cmake --build build/fuzz
ctest --test-dir build/fuzz                          # 20000 mutations per target
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

- `Archive::className` indexed the object table with `none` when a class's
  `$classname` was itself a UID resolving to `$null` or past the archive: an
  out-of-bounds read on a malformed QLab workspace. Fixed; pinned in
  `BplistTests.cpp`.
