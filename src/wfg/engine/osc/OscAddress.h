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
    What an OSC address may look like.

    The spec reserves nine printable characters in an address: space, `#`, `*`,
    `,`, `/` (inside a part), `?`, `[`, `]`, `{`, `}`. Four of them are the
    pattern-matching wildcards, one starts a bundle, one separates type tags and
    one separates parts. Everything else printable is a legal character in a
    part name, which is why Crockford base32 identifiers are usable verbatim as
    addresses.

    A PATTERN IS TOLD APART FROM AN ADDRESS, and that distinction earns its
    keep. An address with a wildcard in the middle of it - a star where a cue
    identifier would go - is a perfectly legal OSC address PATTERN, and Go.dot
    does not dispatch patterns: Phase 1 resolves an address to exactly one node.
    A client that sends one should be told that, rather than have the engine
    look for a node literally named after the wildcard and report that no such
    address exists. So the two questions are asked separately and the rejection
    can say which it was.

    NON-ASCII IS NOT AN ADDRESS. OSC strings are ASCII, and a byte above 0x7E in
    an address is a sender that has encoded something it should not have. Go.dot
    refuses it rather than passing it through: an address is a key into a tree,
    and a key that renders differently on two machines is not a key.

    Vendor-free, and pure - no state, no allocation beyond what a caller asks
    for.
*/

#include <string>
#include <string_view>
#include <vector>

namespace wfg::osc
{
    /*  A well-formed, LITERAL OSC address: begins with `/`, has no empty part,
        and contains none of the nine reserved characters.

        `"/"` is legal and names the root, which is what an OSCQuery client
        means by `GET /`. */
    bool isValidAddress (std::string_view address);

    /*  A well-formed OSC address PATTERN: the same rules, except that `*`, `?`,
        `[`, `]`, `{` and `}` are allowed because the spec gives them a meaning.

        Every valid address is also a valid pattern. */
    bool isValidPattern (std::string_view address);

    /*  True when the string contains a character the spec treats as a wildcard.

        Asked separately from validity so that a rejection can say WHICH thing
        was wrong. A client that puts a star where a cue identifier belongs has
        not sent a bad address; it has sent a pattern to an engine that does not
        dispatch them. */
    bool containsWildcard (std::string_view address);

    /** The parts between the separators, with no empty entries. */
    std::vector<std::string> partsOf (std::string_view address);
}
