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
    THE FUZZ DRIVER, for builds without libFuzzer.

        <target> FILE-OR-DIR...        replay each file once (a regression run)
        <target> --mutate N [SEED]     N deterministic mutations of seeds()
        <target> --write-seeds DIR     write seeds() out as a starting corpus

    Mutations are bit flips, byte sets, inserts, deletes, truncations,
    splices and block copies - the kinds of damage a truncated or corrupt
    file or datagram actually has. The PRNG is fixed, so a failure reproduces
    from its seed number; the failing input is written next to the working
    directory as `crash-input` before the target runs, so a trap leaves it behind.
*/

#include "FuzzTarget.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace
{
    namespace fs = std::filesystem;
    using wfg::fuzz::Bytes;

    struct Rng
    {
        std::uint64_t s;
        std::uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
        std::size_t below (std::size_t n) { return n == 0 ? 0 : static_cast<std::size_t> (next() % n); }
    };

    Bytes readFile (const fs::path& p)
    {
        std::ifstream in (p, std::ios::binary);
        return Bytes (std::istreambuf_iterator<char> (in), {});
    }

    void run (const Bytes& b) { LLVMFuzzerTestOneInput (b.data(), b.size()); }

    void mutate (Bytes& b, Rng& r, const std::vector<Bytes>& pool)
    {
        const auto edits = 1 + r.below (4);
        for (std::size_t e = 0; e < edits; ++e)
        {
            switch (r.below (8))
            {
                case 0: if (! b.empty()) b[r.below (b.size())] ^= static_cast<std::uint8_t> (1u << r.below (8)); break;
                case 1: if (! b.empty()) b[r.below (b.size())] = static_cast<std::uint8_t> (r.next()); break;
                case 2: b.insert (b.begin() + static_cast<std::ptrdiff_t> (r.below (b.size() + 1)), static_cast<std::uint8_t> (r.next())); break;
                case 3: if (! b.empty()) b.erase (b.begin() + static_cast<std::ptrdiff_t> (r.below (b.size()))); break;
                case 4: b.resize (r.below (b.size() + 1)); break;
                case 5:   // an extreme value where a length or a count would be
                {
                    static constexpr std::uint8_t extreme[] { 0x00, 0x01, 0x7f, 0x80, 0xff };
                    if (b.size() >= 4)
                    {
                        const auto at = r.below (b.size() - 3);
                        const auto v = extreme[r.below (5)];
                        for (std::size_t i = 0; i < 4; ++i) b[at + i] = v;
                    }
                    break;
                }
                case 6:   // a block copied over another
                    if (b.size() > 2)
                    {
                        const auto len = 1 + r.below (std::min<std::size_t> (16, b.size() - 1));
                        const auto from = r.below (b.size() - len + 1), to = r.below (b.size() - len + 1);
                        std::copy_n (b.begin() + static_cast<std::ptrdiff_t> (from), len, b.begin() + static_cast<std::ptrdiff_t> (to));
                    }
                    break;
                default:  // a splice with another seed
                    if (! pool.empty())
                    {
                        const auto& o = pool[r.below (pool.size())];
                        b.resize (r.below (b.size() + 1));
                        b.insert (b.end(), o.begin() + static_cast<std::ptrdiff_t> (r.below (o.size() + 1)), o.end());
                    }
                    break;
            }
            if (b.size() > (1u << 16)) b.resize (1u << 16);
        }
    }
}

int main (int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "";

    if (mode == "--write-seeds" && argc > 2)
    {
        fs::create_directories (argv[2]);
        int n = 0;
        for (const auto& s : wfg::fuzz::seeds())
        {
            std::ofstream out (fs::path (argv[2]) / ("seed-" + std::to_string (n++)), std::ios::binary);
            out.write (reinterpret_cast<const char*> (s.data()), static_cast<std::streamsize> (s.size()));
        }
        return 0;
    }

    if (mode == "--mutate" && argc > 2)
    {
        const auto count = std::strtoull (argv[2], nullptr, 10);
        Rng rng { argc > 3 ? std::strtoull (argv[3], nullptr, 10) * 2654435761u + 88172645463325252ull
                           : 88172645463325252ull };
        const auto pool = wfg::fuzz::seeds();
        if (pool.empty()) return 1;

        for (const auto& s : pool) run (s);   // the valid ones first, unharmed

        for (std::uint64_t i = 0; i < count; ++i)
        {
            Bytes b = pool[rng.below (pool.size())];
            mutate (b, rng, pool);
            {
                std::ofstream out ("crash-input", std::ios::binary);   // overwritten each run; kept if the next call traps
                out.write (reinterpret_cast<const char*> (b.data()), static_cast<std::streamsize> (b.size()));
            }
            run (b);
        }
        std::remove ("crash-input");
        std::printf ("%llu mutations, no failure\n", static_cast<unsigned long long> (count));
        return 0;
    }

    int files = 0;
    for (int i = 1; i < argc; ++i)
    {
        const fs::path p (argv[i]);
        if (fs::is_directory (p))
            for (const auto& e : fs::recursive_directory_iterator (p)) { if (e.is_regular_file()) { run (readFile (e.path())); ++files; } }
        else if (fs::is_regular_file (p)) { run (readFile (p)); ++files; }
    }
    if (files == 0) { std::fprintf (stderr, "usage: see FuzzDriver.cpp\n"); return 1; }
    std::printf ("%d inputs replayed, no failure\n", files);
    return 0;
}
