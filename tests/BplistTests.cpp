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
    The binary property list and the keyed archive inside one (namespace draft
    §46, ZO, QL.3): every kind of object the format has, read back as written;
    a file that lies about its offsets, counts or references refused rather
    than read past its end; and an archive's UIDs followed - into cycles, to
    `$null`, through archived strings, arrays, dictionaries and data, and into
    an archive held as data inside another, which is how a QLab workspace
    holds its cues.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "PlistWriter.h"

#include <wfg/engine/import/Bplist.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace wfg::import::plist;
namespace writer = wfg::test::plist;

namespace
{
    Parsed parsed (const std::vector<std::uint8_t>& bytes)
    {
        return parse (bytes.data(), bytes.size());
    }

    const Object& member (const PropertyList& list, const Object& dict, const std::string& key)
    {
        for (const auto& [k, v] : dict.entries)
            if (list.objects[k].text == key)
                return list.objects[v];

        FAIL ("no " << key);
        return dict;
    }
}

TEST_CASE ("bplist: every kind of object reads back as it was written")
{
    const auto bytes = writer::write (writer::Value::dict ({
        { "nothing",  writer::Value::null() },
        { "yes",      writer::Value::boolean (true) },
        { "no",       writer::Value::boolean (false) },
        { "minus",    writer::Value::integer (-1234567890123LL) },
        { "real",     writer::Value::number (-30.000762688076193) },
        { "ascii",    writer::Value::string ("/channel/110/mute \\T") },
        { "accented", writer::Value::string ("Tod und das M\xC3\xA4" "dchen \xE2\x80\x9C" "Am I Human\xE2\x80\x9D") },
        { "long",     writer::Value::string (std::string (40, 'x')) },
        { "bytes",    writer::Value::data ({ 0, 1, 2, 255 }) },
        { "uid",      writer::Value::uid (70000) },
        { "list",     writer::Value::array ({ writer::Value::integer (1), writer::Value::string ("two") }) },
    }));

    const auto read = parsed (bytes);
    REQUIRE (read.list.has_value());

    const auto& list = *read.list;
    const auto& top = list.objects[list.top];
    REQUIRE (top.type == Object::Type::dict);

    CHECK (member (list, top, "nothing").type == Object::Type::null);
    CHECK (member (list, top, "yes").boolean);
    CHECK_FALSE (member (list, top, "no").boolean);
    CHECK (member (list, top, "minus").integer == -1234567890123LL);
    CHECK (member (list, top, "real").real == doctest::Approx (-30.000762688076193).epsilon (1e-15));
    CHECK (member (list, top, "ascii").text == "/channel/110/mute \\T");
    CHECK (member (list, top, "accented").text == "Tod und das M\xC3\xA4" "dchen \xE2\x80\x9C" "Am I Human\xE2\x80\x9D");
    CHECK (member (list, top, "long").text == std::string (40, 'x'));
    CHECK (member (list, top, "bytes").data == std::vector<std::uint8_t> { 0, 1, 2, 255 });
    CHECK (member (list, top, "uid").type == Object::Type::uid);
    CHECK (member (list, top, "uid").integer == 70000);

    const auto& items = member (list, top, "list");
    REQUIRE (items.items.size() == 2);
    CHECK (list.objects[items.items[0]].integer == 1);
    CHECK (list.objects[items.items[1]].text == "two");
}

TEST_CASE ("bplist: a file that lies about itself is refused in words, never read past its end")
{
    const auto good = writer::write (writer::Value::array ({ writer::Value::string ("one"), writer::Value::integer (2) }));
    REQUIRE (parsed (good).list.has_value());

    SUBCASE ("not a property list at all")
    {
        const std::vector<std::uint8_t> text { 'h', 'e', 'l', 'l', 'o' };
        CHECK (parsed (text).error == "not a binary property list");
        CHECK (parse (nullptr, 0).error == "not a binary property list");
    }

    SUBCASE ("cut short: every length from the header to the whole")
    {
        for (std::size_t length = 0; length < good.size(); ++length)
        {
            const std::vector<std::uint8_t> cut (good.begin(), good.begin() + static_cast<std::ptrdiff_t> (length));
            const auto read = parsed (cut);
            CHECK_FALSE (read.list.has_value());
            CHECK_FALSE (read.error.empty());
        }
    }

    SUBCASE ("an offset table pointing past the objects")
    {
        auto bad = good;
        const auto table = bad.size() - 32 - 3 * 4;
        bad[table + 3] = 0xFF;      // the first object, at 255
        CHECK (parsed (bad).error == "an object starts outside the property list's objects");
    }

    SUBCASE ("a reference to an object the file does not have")
    {
        auto bad = good;
        bad[8 + 1] = 0x00;
        bad[8 + 2] = 0x09;         // the array's first member, object 9 of 3
        CHECK (parsed (bad).error == "an object refers to one the file does not have");
    }

    SUBCASE ("a count larger than the file")
    {
        auto bad = good;
        bad[8] = 0xAF;             // an array whose length follows
        bad.insert (bad.begin() + 9, { 0x12, 0x7F, 0xFF, 0xFF, 0xFF });
        CHECK_FALSE (parsed (bad).list.has_value());
    }

    SUBCASE ("a trailer whose widths are nought")
    {
        auto bad = good;
        bad[bad.size() - 32 + 6] = 0;
        CHECK_FALSE (parsed (bad).list.has_value());
    }

    SUBCASE ("an array that holds itself is read, not followed")
    {
        //  bplist00, one array of one member: object 0, pointing at object 0.
        std::vector<std::uint8_t> self { 'b', 'p', 'l', 'i', 's', 't', '0', '0', 0xA1, 0x00 };
        const auto table = self.size();
        self.push_back (8);

        for (int i = 0; i < 6; ++i)
            self.push_back (0);

        self.push_back (1);
        self.push_back (1);

        for (const std::uint64_t field : { std::uint64_t { 1 }, std::uint64_t { 0 }, static_cast<std::uint64_t> (table) })
            for (int i = 7; i >= 0; --i)
                self.push_back (static_cast<std::uint8_t> (field >> (8 * i)));

        const auto read = parsed (self);
        REQUIRE (read.list.has_value());
        CHECK (read.list->objects[0].items == std::vector<std::size_t> { 0 });
    }
}

TEST_CASE ("keyed archive: UIDs followed to what they name, $null to nothing, cycles harmless")
{
    writer::Keyed keyed;

    const auto name = keyed.string ("Console");
    const auto group = keyed.object ("GroupCue", {});
    const auto child = keyed.object ("OSCCue", { { "parent", writer::Value::uid (group) },
                                                 { "rawString", writer::Value::string ("/channel/110/fader 0.") },
                                                 { "patch", writer::Value::integer (3) },
                                                 { "preWait", writer::Value::number (0.5) },
                                                 { "armed", writer::Value::boolean (true) },
                                                 { "notes", writer::Value::uid (0) } });
    const auto children = keyed.array ({ child });
    const auto note = keyed.object ("NSAttributedString", { { "NSString", writer::Value::uid (keyed.string ("Doors open")) } });
    const auto names = keyed.dictionary ({ { keyed.add (writer::Value::integer (2)), keyed.add (writer::Value::string ("WFS")) } });

    keyed.set (group, writer::Value::dict ({ { "$class", keyed.at (group).entries.front().second },
                                             { "name", writer::Value::uid (name) },
                                             { "cues", writer::Value::uid (children) },
                                             { "notes", writer::Value::uid (note) },
                                             { "names", writer::Value::uid (names) },
                                             { "inner", writer::Value::uid (keyed.data ({ 1, 2, 3 })) } }));

    const auto bytes = keyed.archive (group);
    auto read = parsed (bytes);
    REQUIRE (read.list.has_value());

    std::string error;
    const auto archive = Archive::from (std::move (*read.list), error);
    REQUIRE (archive.has_value());

    const auto root = archive->top();
    CHECK (archive->className (root) == "GroupCue");
    CHECK (archive->text (archive->field (root, "name")) == std::optional<std::string> ("Console"));
    CHECK (archive->text (archive->field (root, "notes")) == std::optional<std::string> ("Doors open"));
    CHECK (archive->isNull (archive->field (root, "nothing here")));

    const auto members = archive->items (archive->field (root, "cues"));
    REQUIRE (members.size() == 1);

    const auto cue = members.front();
    CHECK (archive->className (cue) == "OSCCue");
    CHECK (archive->text (archive->field (cue, "rawString")) == std::optional<std::string> ("/channel/110/fader 0."));
    CHECK (archive->integer (archive->field (cue, "patch")) == std::optional<std::int64_t> (3));
    REQUIRE (archive->number (archive->field (cue, "preWait")).has_value());
    CHECK (*archive->number (archive->field (cue, "preWait")) == doctest::Approx (0.5));
    CHECK (archive->boolean (archive->field (cue, "armed")) == std::optional<bool> (true));
    CHECK (archive->isNull (archive->field (cue, "notes")));

    //  The child points at its parent, which holds the child: a cycle, followed
    //  one step at a time and never all at once.
    CHECK (archive->field (cue, "parent") == root);

    const auto entries = archive->entries (archive->field (root, "names"));
    REQUIRE (entries.size() == 1);
    CHECK (entries.front().first == "2");
    CHECK (archive->text (entries.front().second) == std::optional<std::string> ("WFS"));

    const auto* data = archive->bytes (archive->field (root, "inner"));
    REQUIRE (data != nullptr);
    CHECK (*data == std::vector<std::uint8_t> { 1, 2, 3 });
}

TEST_CASE ("keyed archive: an archive held as data inside another, as a QLab workspace holds its cues")
{
    writer::Keyed inner;
    const auto cue = inner.object ("MemoCue", { { "name", writer::Value::string ("TUNE") } });
    const auto innerBytes = inner.archive (cue);

    writer::Keyed outer;
    const auto data = outer.data (innerBytes);
    const auto root = outer.dictionary ({ { outer.add (writer::Value::string ("cueLists")), data } });
    auto read = parsed (outer.archive (root));
    REQUIRE (read.list.has_value());

    std::string error;
    const auto outerArchive = Archive::from (std::move (*read.list), error);
    REQUIRE (outerArchive.has_value());

    const auto entries = outerArchive->entries (outerArchive->top());
    REQUIRE (entries.size() == 1);

    const auto* held = outerArchive->bytes (entries.front().second);
    REQUIRE (held != nullptr);

    auto innerRead = parse (held->data(), held->size());
    REQUIRE (innerRead.list.has_value());

    const auto innerArchive = Archive::from (std::move (*innerRead.list), error);
    REQUIRE (innerArchive.has_value());
    CHECK (innerArchive->className (innerArchive->top()) == "MemoCue");
    CHECK (innerArchive->text (innerArchive->field (innerArchive->top(), "name")) == std::optional<std::string> ("TUNE"));
}

TEST_CASE ("keyed archive: a list that is not one is refused in words")
{
    std::string error;

    auto plain = parsed (writer::write (writer::Value::array ({})));
    REQUIRE (plain.list.has_value());
    CHECK_FALSE (Archive::from (std::move (*plain.list), error).has_value());
    CHECK (error == "not a keyed archive: its top is not a dictionary");

    auto noObjects = parsed (writer::write (writer::Value::dict ({ { "$top", writer::Value::dict ({}) } })));
    REQUIRE (noObjects.list.has_value());
    CHECK_FALSE (Archive::from (std::move (*noObjects.list), error).has_value());
    CHECK (error == "not a keyed archive: no $objects or no $top");
}
