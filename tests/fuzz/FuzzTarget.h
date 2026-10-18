/*
    This file is part of Go.dot — https://github.com/pob31/go.dot

    Copyright (C) 2026 Pierre-Olivier Boulant

    Go.dot is free software: you can redistribute it and/or modify it under the
    terms of the GNU General Public License as published by the Free Software
    Foundation, either version 3 of the License, or (at your option) any later
    version. Go.dot is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
    or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
    (LICENSE, at the repository root) for more details.

    SPDX-License-Identifier: GPL-3.0-or-later
*/

/*
    THE FUZZ TARGETS' SHARED SHAPE.

    Each target file defines `LLVMFuzzerTestOneInput` (the libFuzzer entry
    point: any bytes in, return 0, never crash, never hang) and `seeds()`, a
    handful of VALID inputs for the mutator to start from. Built against
    libFuzzer, `seeds()` is unused and the corpus comes from `--write-seeds`.
    Built without it (FuzzDriver.cpp), a small deterministic mutator runs the
    same entry point, so the targets work on any compiler under
    ASan + UBSan.

    A target's job is the contract its header states: "never throws, never
    reads outside the buffer". Where the module has an oracle (OSC: what
    decodes must encode and decode again to the same packet), the target
    checks it and `__builtin_trap`s on a break, so a fuzzer reports it as a
    crash.
*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput (const std::uint8_t* data, std::size_t size);

namespace wfg::fuzz
{
    using Bytes = std::vector<std::uint8_t>;

    /** Valid inputs for the mutator to start from. */
    std::vector<Bytes> seeds();
}
