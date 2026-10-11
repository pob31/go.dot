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

#include <wfg/engine/cue/RunNames.h>

#include <utility>

namespace wfg::cue
{
    namespace
    {
        constexpr std::uint64_t fnvOffset = 14695981039346656037ull;
        constexpr std::uint64_t fnvPrime = 1099511628211ull;

        /*  A THOUSAND ATTEMPTS, as `IdRegistry::generate` allows itself: with
            forty bits and a show's few thousand runs the first attempt is the
            one, and the bound is only there so a broken state cannot spin. */
        constexpr std::uint64_t attempts = 1000;

        std::uint64_t mixByte (std::uint64_t hash, std::uint8_t byte) noexcept
        {
            return (hash ^ byte) * fnvPrime;
        }

        std::uint64_t mixText (std::uint64_t hash, std::string_view text) noexcept
        {
            for (const auto c : text)
                hash = mixByte (hash, static_cast<std::uint8_t> (c));

            /*  AND A BYTE NO IDENTIFIER HOLDS AFTER IT, so `AB` then `C` and
                `A` then `BC` are two causes and not one. */
            return mixByte (hash, 0x1f);
        }

        std::uint64_t mixNumber (std::uint64_t hash, std::uint64_t value) noexcept
        {
            for (int shift = 0; shift < 64; shift += 8)
                hash = mixByte (hash, static_cast<std::uint8_t> ((value >> shift) & 0xffu));

            return hash;
        }

        /*  splitmix64's finisher. FNV-1a's multiply carries a byte's change
            upward only, so the last bytes in - the count and the attempt -
            would otherwise reach the low bits a name keeps through only a few
            of theirs. */
        std::uint64_t finish (std::uint64_t z) noexcept
        {
            z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
            z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
            return z ^ (z >> 31);
        }
    }

    RunNames::RunNames (const RunTable& runsToRead, doc::IdRegistry& idsToReserve)
        : runs (runsToRead), ids (idsToReserve)
    {
    }

    std::string RunNames::causeOf (std::string_view parentRun, std::string_view cueId)
    {
        if (! parentRun.empty())
            return std::string (parentRun);

        return "root:" + std::string (cueId);
    }

    std::string RunNames::candidate (std::uint64_t salt, std::string_view cause, std::string_view cue,
                                     std::uint64_t n, std::uint64_t attempt)
    {
        auto hash = mixNumber (fnvOffset, salt);
        hash = mixText (hash, cause);
        hash = mixText (hash, cue);
        hash = mixNumber (hash, n);
        hash = mixNumber (hash, attempt);
        return doc::Id::encode (finish (hash));
    }

    std::uint64_t RunNames::countOf (std::string_view cause, std::string_view cue) const
    {
        const auto found = counts.find ({ std::string (cause), std::string (cue) });
        return found != counts.end() ? found->second : 0;
    }

    std::string RunNames::drawn (std::uint64_t salt, std::string_view cause, std::string_view cue) const
    {
        const auto n = countOf (cause, cue);

        for (std::uint64_t attempt = 0; attempt < attempts; ++attempt)
        {
            auto name = candidate (salt, cause, cue, n, attempt);

            if (doc::IdRegistry::readAsNumberByPd (name))
                continue;

            if (runs.find (name) != nullptr || ids.isTaken (name))
                continue;

            return name;
        }

        return {};
    }

    std::string RunNames::next (std::string_view cause, std::string_view cue) const
    {
        return drawn (currentSalt, cause, cue);
    }

    std::string RunNames::nextUnsalted (std::string_view cause, std::string_view cue) const
    {
        return drawn (0, cause, cue);
    }

    void RunNames::made (std::string_view cause, std::string_view cue, const std::string& name)
    {
        count (cause, cue);

        /*  RESERVED WHETHER DRAWN OR HANDED IN, so the registry holds the same
            names on a machine that drew them and on one a record gave them to.
            A name that is not an identifier - a test's, a client's - reserves
            nothing, and is still counted. */
        ids.reserve (name);
    }

    std::string RunNames::drawnIn (std::uint64_t salt, const doc::IdRegistry& registry,
                                   std::string_view cause, std::string_view what) const
    {
        const auto n = countOf (cause, what);

        for (std::uint64_t attempt = 0; attempt < attempts; ++attempt)
        {
            auto name = candidate (salt, cause, what, n, attempt);

            if (! doc::IdRegistry::readAsNumberByPd (name) && ! registry.isTaken (name))
                return name;
        }

        return {};
    }

    std::string RunNames::nextIn (const doc::IdRegistry& registry,
                                  std::string_view cause, std::string_view what) const
    {
        return drawnIn (currentSalt, registry, cause, what);
    }

    std::string RunNames::nextInUnsalted (const doc::IdRegistry& registry,
                                          std::string_view cause, std::string_view what) const
    {
        return drawnIn (0, registry, cause, what);
    }

    void RunNames::count (std::string_view cause, std::string_view what)
    {
        ++counts[{ std::string (cause), std::string (what) }];
    }

    std::int32_t RunNames::seedFor (std::string_view cause) const
    {
        auto hash = mixNumber (fnvOffset, currentSalt);
        hash = mixText (hash, "seed");
        hash = mixText (hash, cause);

        const auto seed = static_cast<std::int32_t> (finish (hash) & 0x7fffffffu);
        return seed == 0 ? 1 : seed;
    }

    void RunNames::seat (Position wanted)
    {
        currentSalt = wanted.salt;
        counts = std::move (wanted.counts);
    }
}
