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

#pragma once

/*
    Machine identity for the things a show is made of (PRD 3.5).

    Eight characters of Crockford base32, upper case: 0-9 and A-Z without I, L,
    O and U. The excluded four are the ones that get misread aloud or in
    handwriting, which matters because this string is meant to be VISIBLE,
    copyable and searchable - a stage manager may end up reading one over comms,
    and Choufleur (3.23) points at cues by exactly this string.

    Forty bits from the system's entropy source. Not a counter, and not derived
    from anything in the document: two shows edited in parallel and later merged
    must not have colliding identifiers, and a counter guarantees that they do.

    NO TOMBSTONES (settled by the author, 2026-09-05: "reusing is not such a
    problem, we can skip tombstones"). Deletion forgets. So the guarantee is
    stated carefully rather than overclaimed: an identifier is unique among the
    objects that EXIST, and a new one is drawn from 2^40, so reissuing a value
    some deleted object once held is possible and vanishingly unlikely. PRD
    3.5's "never reused" is honoured in practice and not enforced in the file.

    Cue NUMBERS are a different thing entirely and live in the document as
    ordinary strings: mutable, decimal, renumbered every tech, and never an
    identity.
*/

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace wfg::doc
{
    class Id
    {
    public:
        static constexpr std::size_t length = 8;

        /** The alphabet, in value order: index is the digit's value. */
        static constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

        /** True if `text` is a well-formed identifier. Case-sensitive: the
            canonical form is upper case, and accepting both would mean two
            spellings of one identity. */
        static bool isValid (std::string_view text) noexcept;

        /** Encodes 40 bits. Only the low 40 are used; anything above is
            ignored rather than silently changing the length. */
        static std::string encode (std::uint64_t value);

        /** The inverse, or nullopt if `text` is not a valid identifier. */
        static std::optional<std::uint64_t> decode (std::string_view text) noexcept;
    };

    /*  Draws identifiers and remembers which are taken.

        `generate` is the only place in the engine that consumes randomness, and
        the value it returns is written into the event log as the argument the
        caller left out - so a replay re-supplies it rather than drawing again,
        and nothing downstream has to be deterministic in order for replay to
        be. */
    class IdRegistry
    {
    public:
        IdRegistry();

        /** Seeds from the system entropy source. */
        static IdRegistry withSystemEntropy();

        /** A fixed seed, for tests that want to name the identifiers they get.
            Never used by the engine itself. */
        static IdRegistry withSeed (std::uint64_t seed);

        /** A fresh identifier, not currently in use, and now reserved. */
        std::string generate();

        /** Reserves an identifier read from a file or replayed from a log.
            False if it was already taken, which is a duplicate in the document
            and a validation error rather than something to paper over. */
        bool reserve (std::string_view id);

        /** Gives an identifier back, because its object was deleted. It may be
            drawn again; see the note about tombstones at the top of this file. */
        void release (std::string_view id);

        bool isTaken (std::string_view id) const;

        std::size_t size() const noexcept { return taken.size(); }

        void clear();

    private:
        explicit IdRegistry (std::uint64_t seed);

        std::uint64_t nextRandom();

        std::unordered_set<std::string> taken;
        std::uint64_t state = 0;
    };
}
