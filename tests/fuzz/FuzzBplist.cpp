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
    FUZZ TARGET: the binary property list and the keyed archive inside one
    (Bplist.h: "every offset, count and reference is checked against the
    bytes before it is used; a file that lies about any of them is refused in
    words, never read past its end").

    A file that parses is then READ THE WAY THE QLAB IMPORT READS IT: every
    object asked for every accessor, the `root` walked through `items`,
    `entries` and `field` (the graph may be cyclic, so each handle is visited
    once), and any data an archive holds parsed again, which is how a QLab
    workspace holds its cues.
*/

#include "FuzzTarget.h"

#include "../PlistWriter.h"

#include <wfg/engine/import/Bplist.h>

#include <set>
#include <string>

namespace plist = wfg::import::plist;

namespace
{
    void touch (const plist::Archive& archive, plist::Archive::Handle h, std::set<plist::Archive::Handle>& seen, int& nested)
    {
        if (archive.isNull (h) || ! seen.insert (h).second)
            return;

        (void) archive.className (h);
        (void) archive.number (h);
        (void) archive.integer (h);
        (void) archive.boolean (h);
        (void) archive.text (h);
        (void) archive.field (h, "NS.string");

        if (const auto* bytes = archive.bytes (h); bytes != nullptr && nested < 2)
        {
            ++nested;
            LLVMFuzzerTestOneInput (bytes->data(), bytes->size());   // an archive held as data
        }

        for (const auto item : archive.items (h))
            touch (archive, item, seen, nested);

        for (const auto& [key, value] : archive.entries (h))
        {
            (void) key;
            touch (archive, value, seen, nested);
        }

        for (const auto* key : { "$class", "NS.objects", "NS.keys", "children", "cues", "name" })
            touch (archive, archive.field (h, key), seen, nested);
    }
}

extern "C" int LLVMFuzzerTestOneInput (const std::uint8_t* data, std::size_t size)
{
    static thread_local int depth = 0;
    if (depth >= 3)
        return 0;
    ++depth;

    auto parsed = plist::parse (data, size);

    if (! parsed.list)
    {
        if (parsed.error.empty())
            __builtin_trap();   // refused "in words", or not refused
        --depth;
        return 0;
    }

    // Every reference the flat table holds points inside the table.
    const auto count = parsed.list->objects.size();
    if (parsed.list->top >= count)
        __builtin_trap();
    for (const auto& o : parsed.list->objects)
    {
        for (const auto i : o.items)
            if (i >= count) __builtin_trap();
        for (const auto& [k, v] : o.entries)
            if (k >= count || v >= count) __builtin_trap();
    }

    std::string error;
    if (auto archive = plist::Archive::from (std::move (*parsed.list), error))
    {
        std::set<plist::Archive::Handle> seen;
        int nested = 0;
        touch (*archive, archive->top(), seen, nested);

        for (plist::Archive::Handle h = 0; h < archive->list().objects.size() && h < 4096; ++h)
            touch (*archive, h, seen, nested);
    }

    --depth;
    return 0;
}

namespace wfg::fuzz
{
    namespace writer = wfg::test::plist;

    std::vector<Bytes> seeds()
    {
        std::vector<Bytes> out;

        out.push_back (writer::write (writer::Value::dict ({
            { "a", writer::Value::integer (-7) },
            { "b", writer::Value::number (1.5) },
            { "c", writer::Value::string ("text") },
            { "d", writer::Value::string ("t\xc3\xa9xt") },
            { "e", writer::Value::data ({ 1, 2, 3 }) },
            { "f", writer::Value::array ({ writer::Value::boolean (true), writer::Value::null(), writer::Value::uid (3) }) } })));

        writer::Keyed k;
        const auto name = k.string ("Cue 1");
        const auto child = k.object ("QLabCue", { { "name", writer::Value::uid (name) }, { "level", writer::Value::number (0.5) } });
        const auto list = k.array ({ child, name });
        const auto root = k.object ("QLabWorkspace", { { "cues", writer::Value::uid (list) } });
        k.set (child, writer::Value::dict ({ { "$class", writer::Value::uid (1) }, { "parent", writer::Value::uid (root) } }));   // a cycle
        out.push_back (k.archive (root));

        writer::Keyed inner;
        const auto innerRoot = inner.object ("Inner", { { "n", writer::Value::integer (1) } });
        writer::Keyed outer;
        const auto held = outer.data (inner.archive (innerRoot));
        out.push_back (outer.archive (outer.object ("Outer", { { "payload", writer::Value::uid (held) } })));

        return out;
    }
}
