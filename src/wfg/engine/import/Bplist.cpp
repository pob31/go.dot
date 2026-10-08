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

#include <wfg/engine/import/Bplist.h>

#include <cmath>
#include <cstring>

namespace wfg::import::plist
{
    namespace
    {
        /*  THE READER OF ONE FILE: the bytes, and what the trailer said about
            them. Every read checks its bounds first; the first thing that does
            not fit is the error, and nothing after it is read. */
        struct Reader
        {
            const std::uint8_t* bytes = nullptr;
            std::size_t size = 0;
            std::string error;

            unsigned refSize = 0;
            std::uint64_t count = 0;
            std::uint64_t tableStart = 0;
            std::vector<std::uint64_t> offsets;

            bool fail (std::string why)
            {
                if (error.empty())
                    error = std::move (why);

                return false;
            }

            /*  A BIG-ENDIAN UNSIGNED NUMBER of `width` bytes at `at`, the width
                of an offset or a reference - one to eight. */
            bool unsignedAt (std::uint64_t at, unsigned width, std::uint64_t& out)
            {
                if (width == 0 || width > 8 || at > size || size - at < width)
                    return fail ("a number runs past the end of the file");

                out = 0;

                for (unsigned i = 0; i < width; ++i)
                    out = (out << 8) | bytes[at + i];

                return true;
            }

            /*  A LENGTH: the marker's low nibble, or - when that is 15 - the
                integer object that follows it. `at` moves past whatever was
                read. */
            bool lengthAt (std::uint8_t marker, std::uint64_t& at, std::uint64_t& out)
            {
                const auto low = static_cast<unsigned> (marker & 0x0F);

                if (low != 0x0F)
                {
                    out = low;
                    return true;
                }

                if (at >= size)
                    return fail ("a length runs past the end of the file");

                const auto intMarker = bytes[at];

                if ((intMarker & 0xF0) != 0x10 || (intMarker & 0x0F) > 3)
                    return fail ("a length is not an integer");

                const auto width = 1u << (intMarker & 0x0F);

                if (! unsignedAt (at + 1, width, out))
                    return false;

                at += 1 + width;
                return true;
            }

            /*  A REFERENCE TO ANOTHER OBJECT, checked to name one that exists. */
            bool refAt (std::uint64_t at, std::size_t& out)
            {
                std::uint64_t value = 0;

                if (! unsignedAt (at, refSize, value))
                    return false;

                if (value >= count)
                    return fail ("an object refers to one the file does not have");

                out = static_cast<std::size_t> (value);
                return true;
            }

            /*  ROOM FOR `n` THINGS OF `width` BYTES from `at`, before the offset
                table - asked before a count is trusted to size anything. */
            bool roomFor (std::uint64_t at, std::uint64_t n, std::uint64_t width)
            {
                if (at > tableStart || (width != 0 && n > (tableStart - at) / width))
                    return fail ("an object says it holds more than the file has room for");

                return true;
            }

            bool object (std::size_t index, Object& out);
        };

        /*  UTF-16 BIG-ENDIAN TO UTF-8, a surrogate pair joined, a lone half
            written as U+FFFD rather than refused - a name with one odd
            character is still a name. */
        std::string utf8FromUtf16 (const std::uint8_t* at, std::size_t units)
        {
            std::string out;
            out.reserve (units);

            const auto put = [&out] (std::uint32_t code)
            {
                if (code < 0x80)
                {
                    out += static_cast<char> (code);
                }
                else if (code < 0x800)
                {
                    out += static_cast<char> (0xC0 | (code >> 6));
                    out += static_cast<char> (0x80 | (code & 0x3F));
                }
                else if (code < 0x10000)
                {
                    out += static_cast<char> (0xE0 | (code >> 12));
                    out += static_cast<char> (0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char> (0x80 | (code & 0x3F));
                }
                else
                {
                    out += static_cast<char> (0xF0 | (code >> 18));
                    out += static_cast<char> (0x80 | ((code >> 12) & 0x3F));
                    out += static_cast<char> (0x80 | ((code >> 6) & 0x3F));
                    out += static_cast<char> (0x80 | (code & 0x3F));
                }
            };

            for (std::size_t i = 0; i < units; ++i)
            {
                const std::uint32_t unit = (static_cast<std::uint32_t> (at[2 * i]) << 8) | at[2 * i + 1];

                if (unit >= 0xD800 && unit <= 0xDBFF && i + 1 < units)
                {
                    const std::uint32_t low = (static_cast<std::uint32_t> (at[2 * i + 2]) << 8) | at[2 * i + 3];

                    if (low >= 0xDC00 && low <= 0xDFFF)
                    {
                        put (0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                        ++i;
                        continue;
                    }
                }

                put (unit >= 0xD800 && unit <= 0xDFFF ? 0xFFFD : unit);
            }

            return out;
        }

        bool Reader::object (std::size_t index, Object& out)
        {
            auto at = offsets[index];
            const auto marker = bytes[at];
            ++at;

            const auto kind = marker >> 4;
            const auto low = static_cast<unsigned> (marker & 0x0F);

            switch (kind)
            {
                case 0x0:
                    if (marker == 0x08 || marker == 0x09)
                    {
                        out.type = Object::Type::boolean;
                        out.boolean = marker == 0x09;
                        return true;
                    }

                    if (marker == 0x00 || marker == 0x0F)
                    {
                        out.type = Object::Type::null;
                        return true;
                    }

                    return fail ("an object of a kind the format does not have");

                case 0x1:
                {
                    if (low > 4)
                        return fail ("an integer wider than sixteen bytes");

                    /*  SIXTEEN BYTES is how a negative number too wide for eight
                        is written; its low eight are the value. */
                    const auto width = 1u << low;
                    std::uint64_t value = 0;

                    if (! unsignedAt (width == 16 ? at + 8 : at, width == 16 ? 8 : width, value))
                        return false;

                    out.type = Object::Type::integer;
                    out.integer = static_cast<std::int64_t> (value);
                    return true;
                }

                case 0x2:
                case 0x3:
                {
                    const auto width = kind == 0x3 ? 8u : 1u << low;
                    std::uint64_t raw = 0;

                    if ((width != 4 && width != 8) || ! unsignedAt (at, width, raw))
                        return fail ("a real that is neither four bytes nor eight");

                    if (width == 4)
                    {
                        const auto narrow = static_cast<std::uint32_t> (raw);
                        float value = 0.0f;
                        std::memcpy (&value, &narrow, sizeof value);
                        out.real = value;
                    }
                    else
                    {
                        double value = 0.0;
                        std::memcpy (&value, &raw, sizeof value);
                        out.real = value;
                    }

                    out.type = kind == 0x3 ? Object::Type::date : Object::Type::real;
                    return true;
                }

                case 0x4:
                case 0x5:
                case 0x6:
                {
                    std::uint64_t length = 0;

                    if (! lengthAt (marker, at, length))
                        return false;

                    const auto unit = kind == 0x6 ? 2u : 1u;

                    if (! roomFor (at, length, unit))
                        return false;

                    const auto* start = bytes + at;
                    const auto n = static_cast<std::size_t> (length);

                    if (kind == 0x4)
                    {
                        out.type = Object::Type::data;
                        out.data.assign (start, start + n);
                    }
                    else
                    {
                        out.type = Object::Type::string;
                        out.text = kind == 0x5 ? std::string (reinterpret_cast<const char*> (start), n)
                                               : utf8FromUtf16 (start, n);
                    }

                    return true;
                }

                case 0x8:
                {
                    std::uint64_t value = 0;

                    if (! unsignedAt (at, low + 1, value))
                        return false;

                    out.type = Object::Type::uid;
                    out.integer = static_cast<std::int64_t> (value);
                    return true;
                }

                case 0xA:
                case 0xC:
                case 0xD:
                {
                    std::uint64_t length = 0;

                    if (! lengthAt (marker, at, length))
                        return false;

                    const auto refs = kind == 0xD ? 2 * length : length;

                    if (length > (std::uint64_t { 1 } << 32) || ! roomFor (at, refs, refSize))
                        return false;

                    if (kind == 0xD)
                    {
                        out.type = Object::Type::dict;
                        out.entries.resize (static_cast<std::size_t> (length));

                        for (std::uint64_t i = 0; i < length; ++i)
                            if (! refAt (at + i * refSize, out.entries[i].first)
                                || ! refAt (at + (length + i) * refSize, out.entries[i].second))
                                return false;
                    }
                    else
                    {
                        out.type = kind == 0xA ? Object::Type::array : Object::Type::set;
                        out.items.resize (static_cast<std::size_t> (length));

                        for (std::uint64_t i = 0; i < length; ++i)
                            if (! refAt (at + i * refSize, out.items[i]))
                                return false;
                    }

                    return true;
                }

                default:
                    return fail ("an object of a kind the format does not have");
            }
        }

        bool isClass (const Object& object, std::string_view name)
        {
            return object.type == Object::Type::string && object.text == name;
        }
    }

    //==========================================================================
    Parsed parse (const std::uint8_t* bytes, std::size_t size)
    {
        Parsed parsed;
        Reader reader;
        reader.bytes = bytes;
        reader.size = size;

        /*  THE HEADER AND THE TRAILER: eight bytes saying what this is, and
            thirty-two at the end saying how to read the rest. */
        if (bytes == nullptr || size < 8 + 32 || std::memcmp (bytes, "bplist00", 8) != 0)
        {
            parsed.error = "not a binary property list";
            return parsed;
        }

        const auto* trailer = bytes + size - 32;
        const unsigned offsetSize = trailer[6];
        reader.refSize = trailer[7];

        std::uint64_t top = 0;
        reader.unsignedAt (size - 24, 8, reader.count);
        reader.unsignedAt (size - 16, 8, top);
        reader.unsignedAt (size - 8, 8, reader.tableStart);

        if (offsetSize == 0 || offsetSize > 8 || reader.refSize == 0 || reader.refSize > 8)
        {
            parsed.error = "the property list's trailer gives a width of nought or more than eight";
            return parsed;
        }

        if (reader.count == 0 || top >= reader.count || reader.tableStart < 8 || reader.tableStart > size - 32
            || reader.count > (size - 32 - reader.tableStart) / offsetSize)
        {
            parsed.error = "the property list's trailer does not match its size";
            return parsed;
        }

        reader.offsets.resize (static_cast<std::size_t> (reader.count));

        for (std::uint64_t i = 0; i < reader.count; ++i)
        {
            if (! reader.unsignedAt (reader.tableStart + i * offsetSize, offsetSize, reader.offsets[i]))
                break;

            if (reader.offsets[i] < 8 || reader.offsets[i] >= reader.tableStart)
            {
                reader.fail ("an object starts outside the property list's objects");
                break;
            }
        }

        PropertyList list;
        list.top = static_cast<std::size_t> (top);

        if (reader.error.empty())
        {
            list.objects.resize (static_cast<std::size_t> (reader.count));

            for (std::size_t i = 0; i < list.objects.size(); ++i)
                if (! reader.object (i, list.objects[i]))
                    break;
        }

        if (! reader.error.empty())
        {
            parsed.error = reader.error;
            return parsed;
        }

        parsed.list = std::move (list);
        return parsed;
    }

    //==========================================================================
    Archive::Archive (PropertyList listToUse) : plist (std::move (listToUse)) {}

    std::optional<Archive> Archive::from (PropertyList list, std::string& error)
    {
        Archive archive (std::move (list));
        const auto& objects = archive.plist.objects;
        const auto& root = objects[archive.plist.top];

        if (root.type != Object::Type::dict)
        {
            error = "not a keyed archive: its top is not a dictionary";
            return std::nullopt;
        }

        std::optional<std::size_t> objectsArray, top;

        for (const auto& [key, value] : root.entries)
        {
            if (isClass (objects[key], "$objects") && objects[value].type == Object::Type::array)
                objectsArray = value;
            else if (isClass (objects[key], "$top") && objects[value].type == Object::Type::dict)
                top = value;
        }

        if (! objectsArray.has_value() || ! top.has_value())
        {
            error = "not a keyed archive: no $objects or no $top";
            return std::nullopt;
        }

        archive.archived = objects[*objectsArray].items;
        archive.topDictionary = *top;
        return archive;
    }

    Archive::Handle Archive::resolve (std::size_t objectIndex) const
    {
        const auto& object = plist.objects[objectIndex];

        if (object.type == Object::Type::uid)
        {
            const auto uid = static_cast<std::uint64_t> (object.integer);

            if (uid >= archived.size())
                return none;

            const auto target = archived[static_cast<std::size_t> (uid)];
            const auto& named = plist.objects[target];

            /*  `$null` IS THE ARCHIVER'S NIL, the string at index nought. */
            if (named.type == Object::Type::string && named.text == "$null")
                return none;

            return target;
        }

        return objectIndex;
    }

    Archive::Handle Archive::plainField (Handle handle, std::string_view key) const
    {
        if (handle == none || handle >= plist.objects.size())
            return none;

        const auto& object = plist.objects[handle];

        if (object.type != Object::Type::dict)
            return none;

        for (const auto& [k, v] : object.entries)
            if (plist.objects[k].type == Object::Type::string && plist.objects[k].text == key)
                return v;

        return none;
    }

    Archive::Handle Archive::top (std::string_view key) const
    {
        const auto value = plainField (topDictionary, key);
        return value == none ? none : resolve (value);
    }

    Archive::Handle Archive::field (Handle handle, std::string_view key) const
    {
        const auto value = plainField (handle, key);
        return value == none ? none : resolve (value);
    }

    std::string Archive::className (Handle handle) const
    {
        const auto named = field (handle, "$class");

        if (named == none)
            return {};

        const auto name = plainField (named, "$classname");

        if (name == none)
            return {};

        const auto& text = plist.objects[resolve (name)];
        return text.type == Object::Type::string ? text.text : std::string {};
    }

    std::optional<double> Archive::number (Handle handle) const
    {
        if (handle == none)
            return std::nullopt;

        const auto& object = plist.objects[handle];

        if (object.type == Object::Type::integer)
            return static_cast<double> (object.integer);

        if (object.type == Object::Type::real)
            return object.real;

        if (object.type == Object::Type::boolean)
            return object.boolean ? 1.0 : 0.0;

        return std::nullopt;
    }

    std::optional<std::int64_t> Archive::integer (Handle handle) const
    {
        if (handle == none)
            return std::nullopt;

        const auto& object = plist.objects[handle];

        if (object.type == Object::Type::integer)
            return object.integer;

        if (object.type == Object::Type::boolean)
            return object.boolean ? 1 : 0;

        if (object.type == Object::Type::real && std::isfinite (object.real) && std::abs (object.real) < 9.0e18)
            return static_cast<std::int64_t> (object.real);

        return std::nullopt;
    }

    std::optional<bool> Archive::boolean (Handle handle) const
    {
        const auto value = integer (handle);
        return value.has_value() ? std::optional<bool> (*value != 0) : std::nullopt;
    }

    std::optional<std::string> Archive::text (Handle handle) const
    {
        /*  A FEW STEPS AT MOST - an attributed string holds a string object
            holding the characters - so an archive whose string names itself
            ends here rather than recursing. */
        for (int step = 0; step < 4 && handle != none; ++step)
        {
            const auto& object = plist.objects[handle];

            if (object.type == Object::Type::string)
                return object.text;

            if (object.type != Object::Type::dict)
                return std::nullopt;

            const auto cls = className (handle);

            if (cls == "NSString" || cls == "NSMutableString")
                handle = field (handle, "NS.string");
            else if (cls == "NSAttributedString" || cls == "NSMutableAttributedString")
                handle = field (handle, "NSString");
            else
                return std::nullopt;
        }

        return std::nullopt;
    }

    std::vector<Archive::Handle> Archive::items (Handle handle) const
    {
        std::vector<Handle> out;

        if (handle == none)
            return out;

        const auto& object = plist.objects[handle];
        const std::vector<std::size_t>* members = nullptr;

        if (object.type == Object::Type::array || object.type == Object::Type::set)
            members = &object.items;
        else if (const auto inner = plainField (handle, "NS.objects");
                 inner != none && plist.objects[inner].type == Object::Type::array && plainField (handle, "NS.keys") == none)
            members = &plist.objects[inner].items;

        if (members != nullptr)
            for (const auto member : *members)
                out.push_back (resolve (member));

        return out;
    }

    std::vector<std::pair<std::string, Archive::Handle>> Archive::entries (Handle handle) const
    {
        std::vector<std::pair<std::string, Handle>> out;

        if (handle == none)
            return out;

        const auto keyText = [this] (std::size_t index) -> std::string
        {
            const auto resolved = resolve (index);

            if (resolved == none)
                return {};

            if (const auto words = text (resolved); words.has_value())
                return *words;

            if (const auto whole = integer (resolved); whole.has_value())
                return std::to_string (*whole);

            return {};
        };

        const auto& object = plist.objects[handle];

        if (object.type == Object::Type::dict && plainField (handle, "$class") == none)
        {
            for (const auto& [k, v] : object.entries)
                out.push_back ({ keyText (k), resolve (v) });

            return out;
        }

        const auto keys = plainField (handle, "NS.keys");
        const auto values = plainField (handle, "NS.objects");

        if (keys == none || values == none || plist.objects[keys].type != Object::Type::array
            || plist.objects[values].type != Object::Type::array)
            return out;

        const auto& k = plist.objects[keys].items;
        const auto& v = plist.objects[values].items;

        for (std::size_t i = 0; i < k.size() && i < v.size(); ++i)
            out.push_back ({ keyText (k[i]), resolve (v[i]) });

        return out;
    }

    const std::vector<std::uint8_t>* Archive::bytes (Handle handle) const
    {
        if (handle == none)
            return nullptr;

        const auto& object = plist.objects[handle];

        if (object.type == Object::Type::data)
            return &object.data;

        const auto inner = field (handle, "NS.data");

        if (inner != none && plist.objects[inner].type == Object::Type::data)
            return &plist.objects[inner].data;

        return nullptr;
    }
}
