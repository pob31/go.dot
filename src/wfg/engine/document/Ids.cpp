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

#include <wfg/engine/document/Ids.h>

#include <random>

namespace wfg::doc
{
    namespace
    {
        /*  Digit value, or -1. Crockford's alphabet skips I, L, O and U; this
            accepts only the canonical upper-case spelling, so one identity has
            exactly one string. (Crockford's own decoder folds i and l onto 1
            and o onto 0 when READING human input. That leniency belongs at a
            text field a person types into, not here: two spellings of one
            identifier in a document is a bug waiting for a diff to find it.) */
        int digitValue (char c) noexcept
        {
            const auto pos = Id::alphabet.find (c);
            return pos == std::string_view::npos ? -1 : static_cast<int> (pos);
        }

        constexpr std::uint64_t bitsUsed = 40;
        constexpr std::uint64_t valueMask = (std::uint64_t { 1 } << bitsUsed) - 1;
    }

    //==============================================================================
    bool Id::isValid (std::string_view text) noexcept
    {
        if (text.size() != length)
            return false;

        for (const char c : text)
            if (digitValue (c) < 0)
                return false;

        return true;
    }

    std::string Id::encode (std::uint64_t value)
    {
        value &= valueMask;

        std::string out (length, alphabet[0]);

        /*  Least significant digit last, so the string reads as a base-32
            number and sorting it sorts the value. Eight digits carry 40 bits
            exactly, which is why the mask is what it is. */
        for (std::size_t i = length; i > 0; --i)
        {
            out[i - 1] = alphabet[static_cast<std::size_t> (value & 31u)];
            value >>= 5;
        }

        return out;
    }

    std::optional<std::uint64_t> Id::decode (std::string_view text) noexcept
    {
        if (! isValid (text))
            return std::nullopt;

        std::uint64_t value = 0;

        for (const char c : text)
            value = (value << 5) | static_cast<std::uint64_t> (digitValue (c));

        return value;
    }

    //==============================================================================
    IdRegistry::IdRegistry() : IdRegistry (0) {}

    IdRegistry::IdRegistry (std::uint64_t seed) : state (seed) {}

    IdRegistry IdRegistry::withSystemEntropy()
    {
        /*  std::random_device, mixed into a 64-bit state. On every platform we
            build for it is a real entropy source; where it is not, it is still
            better than a clock, and the consequence of a poor draw here is a
            retry rather than a collision - generate() checks. */
        std::random_device device;

        std::uint64_t seed = 0;

        for (int i = 0; i < 4; ++i)
            seed = (seed << 16) ^ static_cast<std::uint64_t> (device());

        return IdRegistry (seed);
    }

    IdRegistry IdRegistry::withSeed (std::uint64_t seed)
    {
        return IdRegistry (seed);
    }

    std::uint64_t IdRegistry::nextRandom()
    {
        /*  splitmix64. Chosen for two properties that matter here and nowhere
            else in the engine: it is a few lines, so there is nothing to get
            wrong, and it has no bad seeds - a state of zero produces a perfectly
            good stream, which a xorshift would not. Its output is well
            distributed across all 64 bits, so taking the low 40 is sound. */
        state += 0x9e3779b97f4a7c15ull;

        auto z = state;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }

    std::string IdRegistry::generate()
    {
        /*  Draw until the value is free. With 2^40 values and a show holding
            thousands of objects, the loop body runs once; the bound is there so
            that a pathological state cannot spin forever, and reaching it would
            mean the entropy source is broken rather than that the show is full. */
        for (int attempt = 0; attempt < 1000; ++attempt)
        {
            auto candidate = Id::encode (nextRandom());

            if (taken.insert (candidate).second)
                return candidate;
        }

        return {};
    }

    std::int32_t IdRegistry::drawSeed()
    {
        /*  The low 31 bits, so it is never negative: the parameter row says
            0.., a client reads it, and a negative seed would be a value the
            grammar refuses to write back. Zero is reserved for "no seed", so
            a draw that lands there is nudged - one value out of two billion,
            and worth not having a special case for later. */
        const auto drawn = static_cast<std::int32_t> (nextRandom() & 0x7fffffffu);

        return drawn == 0 ? 1 : drawn;
    }

    bool IdRegistry::reserve (std::string_view id)
    {
        if (! Id::isValid (id))
            return false;

        return taken.insert (std::string (id)).second;
    }

    void IdRegistry::release (std::string_view id)
    {
        taken.erase (std::string (id));
    }

    bool IdRegistry::isTaken (std::string_view id) const
    {
        return taken.find (std::string (id)) != taken.end();
    }

    void IdRegistry::clear()
    {
        taken.clear();
    }
}
