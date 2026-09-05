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
    The vocabulary the generated parameter table is written in.

    Split from Schema.h so that SchemaTable.generated.h can include a small,
    stable header instead of the whole schema API — a generated file that has to
    be regenerated because an unrelated interface moved is a generated file
    people start editing by hand.

    Everything here is constexpr and vendor-free: the table is a static array
    with no initialisation order to worry about, and it can be read at compile
    time by anything that wants to.
*/

#include <cstddef>
#include <string_view>

namespace wfg::doc
{
    /*  How a value is stored and written. Narrower than the OSC type set on
        purpose: a document attribute is a decided value, so there is no place
        here for an impulse, a time tag or a nil. The OSC tag each one maps to
        travels alongside, for the tree and the wire. */
    enum class ValueType
    {
        string,
        integer,      ///< i, int32
        integer64,    ///< h
        number,       ///< f or d; the tag says which
        boolean,      ///< T / F
        blob          ///< b; nothing in Phase 1 uses one
    };

    /*  Whether a CLIENT may write it. Independent of whether it is stored:
        a mount's prefix is decided by someone and lives in the file, but
        nothing may write it over OSC while a show is running. `persist` is the
        column that says what reaches a file; conflating the two makes a stored
        read-only attribute unwritable to disk, which was a real bug here. */
    enum class Access { read, write, readWrite };

    /*  PRD §3.3. A settable state has a value at time T; an event is one-shot
        and does not, which is why an event is never persisted and never solved
        for. */
    enum class Kind { state, event };

    /*  Which file the value belongs in, and PRD §4.10 made mechanical:
        `show` is what someone decided, `state` is what the machine happened to
        be doing, `none` never reaches a file at all.

        `none` is also how a DERIVED value is marked - a cue's index among its
        siblings, its kind, a list's order. Those are computed from the tree, so
        storing them would be storing a second copy of something the structure
        already says, and the two would eventually disagree. The schema drops
        them from an element's attribute list entirely, which is what makes the
        writer and the reader refuse them without either having to know why. */
    enum class Persist { none, show, state };

    /*  One row of the parameter table, flattened for a constexpr array.

        The enum values are a pointer and a count rather than a container so the
        whole table can be a single constexpr array with no allocation and no
        static initialisation order. Schema wraps this in something friendlier;
        this is the shape the generator emits. */
    struct AttributeRow
    {
        /*  Who carries it, spelled as the table spells it: engine, document,
            list, cue, group, mount. What that MEANS - which document element it
            lands on, and the fact that a Group inherits every Cue row - is
            Schema's, not the generator's. */
        std::string_view owner;
        std::string_view name;

        ValueType type;
        char oscTypeTag;               ///< the tag the tree and the wire use

        /*  Whether this attribute holds a RUN of that type rather than one of
            it. Written `d*` in the table.

            Everything else on the row describes an ELEMENT: the range bounds
            each value, the unit is each value's unit, the panic value is what
            each one rests at. That is why it is a flag and not a seventh
            ValueType - a `numberList` type would have made every one of those
            columns ambiguous, and the first question anybody asked would have
            been "the range of what?".

            In the document it is a space-separated run, which is XSD's `list`
            and what the generated RELAX NG emits. On the wire it is N arguments
            of `oscTypeTag`, which is what OSC does natively and needs no
            encoding of its own. The length is not declared anywhere: it is
            whatever the document holds, and what makes it correct is the
            element that carries it - `Route/@gains` is C_in x width because the
            cue's channels and the bus's width say so. */
        bool isList;
        Access access;
        Kind kind;
        Persist persist;

        bool hasDefault;
        std::string_view defaultText;  ///< as written in the table, unparsed

        bool hasMin;
        double minimum;
        bool hasMax;
        double maximum;

        const std::string_view* enumValues;
        std::size_t numEnumValues;

        std::string_view unit;
        double rateCap;                ///< Hz
        bool anticipatable;
        std::string_view panic;

        std::string_view description;
    };
}
