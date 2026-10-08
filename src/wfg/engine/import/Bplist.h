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
    A BINARY PROPERTY LIST, AND THE KEYED ARCHIVE INSIDE ONE (namespace draft
    §46, ZO, QL.3): what a QLab workspace is written as, read by Go.dot itself
    on every system rather than by CoreFoundation on one.

    TWO LAYERS.

    `parse` reads Apple's `bplist00` format - a header, the objects, a table of
    where each one starts, and a trailer saying how wide the numbers in that
    table are - into a FLAT TABLE: one `Object` per object in the file, a
    container holding the indices of its members rather than the members. A
    file that points an array at itself is then just an index that repeats,
    and nothing reading it can recurse forever. Every offset, count and
    reference is checked against the bytes before it is used; a file that
    lies about any of them is refused in words, never read past its end.

    `Archive` reads what `NSKeyedArchiver` writes INTO such a list: a dictionary
    whose `$objects` array holds every archived object, each one a dictionary
    naming its class through a `$class` reference, its fields referring to
    one another by `UID` - which is how a QLab cue points at the group it sits
    in, and the group back at the cue. A `Handle` is an index into the flat
    table; `field` follows a UID to the object it names and answers `none`
    for `$null`, so the reader asks for what it wants and never sees a UID.

    STD ONLY, no JUCE: the bytes come in as a pointer and a length.
*/

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::import::plist
{
    struct Object
    {
        enum class Type { null, boolean, integer, real, date, data, string, uid, array, set, dict };

        Type type = Type::null;
        bool boolean = false;
        std::int64_t integer = 0;                   ///< also a UID's value
        double real = 0.0;                          ///< also a date's seconds since 2001
        std::string text;                           ///< a string, as UTF-8
        std::vector<std::uint8_t> data;
        std::vector<std::size_t> items;             ///< an array's or a set's members
        std::vector<std::pair<std::size_t, std::size_t>> entries;   ///< a dictionary's keys and values
    };

    struct PropertyList
    {
        std::vector<Object> objects;
        std::size_t top = 0;
    };

    struct Parsed
    {
        std::optional<PropertyList> list;
        std::string error;                          ///< why not, when `list` is empty
    };

    /** A `bplist00`, every object of it, or why it is not one. */
    Parsed parse (const std::uint8_t* bytes, std::size_t size);

    /*  AN NSKEYEDARCHIVER ARCHIVE, read through handles into its list. */
    class Archive
    {
    public:
        using Handle = std::size_t;
        static constexpr Handle none = static_cast<Handle> (-1);

        /*  The archive in `list`, or nullopt with `error` said: a list that is
            not a dictionary, names no `$objects` array, or no `$top`. */
        static std::optional<Archive> from (PropertyList list, std::string& error);

        /** The object `$top` names under `key` - "root" for every QLab archive. */
        Handle top (std::string_view key = "root") const;

        /** The class an archived object names, empty for one that names none. */
        std::string className (Handle) const;

        /*  ONE FIELD OF AN ARCHIVED OBJECT, a UID followed to what it names:
            `none` for a field the object does not have and for `$null`. */
        Handle field (Handle, std::string_view key) const;

        bool isNull (Handle handle) const { return handle == none; }

        /*  A NUMBER, whatever it was written as: an integer, a real or a
            boolean, and nullopt for anything else - a field QLab wrote as a
            string where it usually writes a number reads as no number. */
        std::optional<double> number (Handle) const;
        std::optional<std::int64_t> integer (Handle) const;
        std::optional<bool> boolean (Handle) const;

        /*  TEXT: a plain string, an `NSString` or `NSMutableString`, or the
            words of an `NSAttributedString` (QLab's notes); nullopt otherwise. */
        std::optional<std::string> text (Handle) const;

        /** The members of an array or a set, plain or archived (`NSArray` and kin). */
        std::vector<Handle> items (Handle) const;

        /*  The entries of a dictionary, plain or archived (`NSDictionary` and
            kin), each key as text - QLab keys some of its dictionaries by
            number, which read here as the number's text. */
        std::vector<std::pair<std::string, Handle>> entries (Handle) const;

        /** The bytes of data, plain or archived (`NSData`, `NSMutableData`); null otherwise. */
        const std::vector<std::uint8_t>* bytes (Handle) const;

        const PropertyList& list() const { return plist; }

    private:
        explicit Archive (PropertyList);

        Handle resolve (std::size_t objectIndex) const;
        Handle plainField (Handle, std::string_view key) const;

        PropertyList plist;
        std::vector<std::size_t> archived;          ///< `$objects`, by UID, as object indices
        std::size_t topDictionary = 0;
    };
}
