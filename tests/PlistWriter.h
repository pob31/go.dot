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
    A BINARY PROPERTY LIST, WRITTEN FOR THE TESTS (namespace draft §46, QL.3):
    the smallest writer that makes the files the decoder reads, so a test can
    build exactly the archive it means - a QLab cue of one field, a reference
    that points nowhere, a list that holds itself - instead of carrying a
    binary fixture nobody can read in a diff.

    `Value` is a tree; `write` lays it out as Apple's `bplist00` does: every
    value an object, objects in the order first met, references two bytes and
    offsets four. `Keyed` builds what NSKeyedArchiver writes on top: an
    `$objects` array, objects naming their class through `$class`, fields
    pointing at one another by UID.

    TEST CODE ONLY. It writes what the tests need and checks nothing.
*/

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace wfg::test::plist
{
    struct Value
    {
        enum class Kind { null, boolean, integer, real, string, data, uid, array, dict } kind = Kind::null;
        bool flag = false;
        std::int64_t whole = 0;
        double real = 0.0;
        std::string text;
        std::vector<std::uint8_t> bytes;
        std::vector<Value> items;
        std::vector<std::pair<std::string, Value>> entries;

        static Value null() { return {}; }
        static Value boolean (bool b) { Value v; v.kind = Kind::boolean; v.flag = b; return v; }
        static Value integer (std::int64_t i) { Value v; v.kind = Kind::integer; v.whole = i; return v; }
        static Value number (double d) { Value v; v.kind = Kind::real; v.real = d; return v; }
        static Value string (std::string s) { Value v; v.kind = Kind::string; v.text = std::move (s); return v; }
        static Value data (std::vector<std::uint8_t> b) { Value v; v.kind = Kind::data; v.bytes = std::move (b); return v; }
        static Value uid (std::int64_t u) { Value v; v.kind = Kind::uid; v.whole = u; return v; }
        static Value array (std::vector<Value> i) { Value v; v.kind = Kind::array; v.items = std::move (i); return v; }
        static Value dict (std::vector<std::pair<std::string, Value>> e) { Value v; v.kind = Kind::dict; v.entries = std::move (e); return v; }
    };

    namespace detail
    {
        inline void bigEndian (std::vector<std::uint8_t>& out, std::uint64_t value, unsigned width)
        {
            for (unsigned i = width; i-- > 0;)
                out.push_back (static_cast<std::uint8_t> (value >> (8 * i)));
        }

        inline void marker (std::vector<std::uint8_t>& out, std::uint8_t kind, std::size_t length)
        {
            if (length < 15)
            {
                out.push_back (static_cast<std::uint8_t> ((static_cast<std::size_t> (kind) << 4) | length));
                return;
            }

            out.push_back (static_cast<std::uint8_t> ((kind << 4) | 0x0F));
            out.push_back (0x12);
            bigEndian (out, length, 4);
        }

        struct Writer
        {
            std::vector<std::vector<std::uint8_t>> objects;

            std::size_t add (const Value& value)
            {
                const auto index = objects.size();
                objects.emplace_back();
                std::vector<std::uint8_t> body;

                switch (value.kind)
                {
                    case Value::Kind::null:    body.push_back (0x00); break;
                    case Value::Kind::boolean: body.push_back (value.flag ? 0x09 : 0x08); break;
                    case Value::Kind::integer: body.push_back (0x13); bigEndian (body, static_cast<std::uint64_t> (value.whole), 8); break;

                    case Value::Kind::real:
                    {
                        body.push_back (0x23);
                        std::uint64_t raw = 0;
                        std::memcpy (&raw, &value.real, sizeof raw);
                        bigEndian (body, raw, 8);
                        break;
                    }

                    case Value::Kind::string:
                    {
                        const auto ascii = std::all_of (value.text.begin(), value.text.end(),
                                                        [] (char c) { return static_cast<unsigned char> (c) < 0x80; });

                        if (ascii)
                        {
                            marker (body, 0x5, value.text.size());
                            body.insert (body.end(), value.text.begin(), value.text.end());
                        }
                        else
                        {
                            /*  UTF-16 for anything else, as Apple writes it -
                                the test strings stay inside the BMP. */
                            std::vector<std::uint16_t> units;

                            for (std::size_t i = 0; i < value.text.size();)
                            {
                                const auto c = static_cast<unsigned char> (value.text[i]);
                                std::uint32_t code = c;
                                int extra = 0;

                                if (c >= 0xE0)      { code = c & 0x0F; extra = 2; }
                                else if (c >= 0xC0) { code = c & 0x1F; extra = 1; }

                                for (int k = 1; k <= extra; ++k)
                                    code = (code << 6) | (static_cast<unsigned char> (value.text[i + static_cast<std::size_t> (k)]) & 0x3F);

                                units.push_back (static_cast<std::uint16_t> (code));
                                i += 1 + static_cast<std::size_t> (extra);
                            }

                            marker (body, 0x6, units.size());

                            for (const auto unit : units)
                                bigEndian (body, unit, 2);
                        }
                        break;
                    }

                    case Value::Kind::data:
                        marker (body, 0x4, value.bytes.size());
                        body.insert (body.end(), value.bytes.begin(), value.bytes.end());
                        break;

                    case Value::Kind::uid:
                        body.push_back (0x83);
                        bigEndian (body, static_cast<std::uint64_t> (value.whole), 4);
                        break;

                    case Value::Kind::array:
                    {
                        std::vector<std::size_t> refs;

                        for (const auto& item : value.items)
                            refs.push_back (add (item));

                        marker (body, 0xA, refs.size());

                        for (const auto ref : refs)
                            bigEndian (body, ref, 2);
                        break;
                    }

                    case Value::Kind::dict:
                    {
                        std::vector<std::size_t> keys, values;

                        for (const auto& [key, item] : value.entries)
                        {
                            keys.push_back (add (Value::string (key)));
                            values.push_back (add (item));
                        }

                        marker (body, 0xD, keys.size());

                        for (const auto ref : keys)
                            bigEndian (body, ref, 2);

                        for (const auto ref : values)
                            bigEndian (body, ref, 2);
                        break;
                    }
                }

                objects[index] = std::move (body);
                return index;
            }
        };
    }

    /** `top` as a binary property list: references of two bytes, offsets of four. */
    inline std::vector<std::uint8_t> write (const Value& top)
    {
        detail::Writer writer;
        writer.add (top);

        std::vector<std::uint8_t> out { 'b', 'p', 'l', 'i', 's', 't', '0', '0' };
        std::vector<std::uint64_t> offsets;

        for (const auto& body : writer.objects)
        {
            offsets.push_back (out.size());
            out.insert (out.end(), body.begin(), body.end());
        }

        const auto table = out.size();

        for (const auto offset : offsets)
            detail::bigEndian (out, offset, 4);

        for (int i = 0; i < 6; ++i)
            out.push_back (0);

        out.push_back (4);
        out.push_back (2);
        detail::bigEndian (out, offsets.size(), 8);
        detail::bigEndian (out, 0, 8);
        detail::bigEndian (out, table, 8);
        return out;
    }

    /*  WHAT NSKEYEDARCHIVER WRITES: `object` adds an archived object of a class
        and answers its UID; a field that is a UID points at another. `$null`
        is UID nought, as the archiver has it. */
    class Keyed
    {
    public:
        Keyed() { objects.push_back (Value::string ("$null")); }

        std::int64_t add (Value value)
        {
            objects.push_back (std::move (value));
            return static_cast<std::int64_t> (objects.size() - 1);
        }

        /*  An object of `className`, its `fields` as given - a UID among them
            pointing at another object. The class is archived once per name. */
        std::int64_t object (const std::string& className, std::vector<std::pair<std::string, Value>> fields)
        {
            fields.insert (fields.begin(), { "$class", Value::uid (classOf (className)) });
            return add (Value::dict (std::move (fields)));
        }

        std::int64_t string (const std::string& text) { return object ("NSString", { { "NS.string", Value::string (text) } }); }

        std::int64_t array (const std::vector<std::int64_t>& uids)
        {
            std::vector<Value> refs;

            for (const auto uid : uids)
                refs.push_back (Value::uid (uid));

            return object ("NSMutableArray", { { "NS.objects", Value::array (std::move (refs)) } });
        }

        std::int64_t dictionary (const std::vector<std::pair<std::int64_t, std::int64_t>>& keysAndValues)
        {
            std::vector<Value> keys, values;

            for (const auto& [k, v] : keysAndValues)
            {
                keys.push_back (Value::uid (k));
                values.push_back (Value::uid (v));
            }

            return object ("NSMutableDictionary", { { "NS.keys", Value::array (std::move (keys)) },
                                                    { "NS.objects", Value::array (std::move (values)) } });
        }

        std::int64_t data (std::vector<std::uint8_t> bytes)
        {
            return object ("NSMutableData", { { "NS.data", Value::data (std::move (bytes)) } });
        }

        /** Replaces what an object holds, for one that must point at another made after it. */
        void set (std::int64_t uid, Value value) { objects[static_cast<std::size_t> (uid)] = std::move (value); }

        Value& at (std::int64_t uid) { return objects[static_cast<std::size_t> (uid)]; }

        std::vector<std::uint8_t> archive (std::int64_t root) const
        {
            return write (Value::dict ({ { "$version", Value::integer (100000) },
                                         { "$archiver", Value::string ("NSKeyedArchiver") },
                                         { "$top", Value::dict ({ { "root", Value::uid (root) } }) },
                                         { "$objects", Value::array (objects) } }));
        }

    private:
        std::int64_t classOf (const std::string& name)
        {
            if (const auto found = classes.find (name); found != classes.end())
                return found->second;

            const auto uid = add (Value::dict ({ { "$classname", Value::string (name) },
                                                 { "$classes", Value::array ({ Value::string (name),
                                                                               Value::string ("NSObject") }) } }));
            classes[name] = uid;
            return uid;
        }

        std::vector<Value> objects;
        std::map<std::string, std::int64_t> classes;
    };
}
