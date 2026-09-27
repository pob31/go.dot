/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/document/CanonicalXml.h>

#include <cstdint>
#include <string>
#include <vector>

using namespace wfg;

TEST_CASE ("group selection: order, descendants and one-step undo are preserved")
{
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\"><Cue id=\"B3N8R5TW\"/>"
        "<Cue id=\"P9XKC2WR\"/><Group id=\"J3MT5XYA\"><Cue id=\"F7HR8TVD\"/></Group>"
        "</List></Lists><Mounts/><Audio tracks=\"2\"/></Show>", document).ok);
    const auto before = doc::CanonicalXml::write (document);
    CHECK_FALSE (document.groupSelection ({ "B3N8R5TW", "missing" }).ok);
    CHECK (doc::CanonicalXml::write (document) == before);
    document.beginTransaction ("group.wrap", 0, "window", {});
    const auto grouped = document.groupSelection ({ "F7HR8TVD", "J3MT5XYA", "B3N8R5TW", "B3N8R5TW" });
    REQUIRE (grouped.ok);
    const auto group = document.findById (grouped.id);
    REQUIRE (group.getNumChildren() == 2);
    CHECK (group.getChild (0)["id"].toString() == "B3N8R5TW");
    CHECK (group.getChild (1)["id"].toString() == "J3MT5XYA");
    CHECK (document.findById ("F7HR8TVD").getParent() == document.findById ("J3MT5XYA"));
    CHECK (document.findById ("P9XKC2WR").getParent() == document.findById ("7K2QM9X4"));
    const auto after = doc::CanonicalXml::write (document);
    REQUIRE (document.undo (doc::UndoDomain::document).has_value());
    CHECK (doc::CanonicalXml::write (document) == before);
    REQUIRE (document.redo (doc::UndoDomain::document).has_value());
    CHECK (doc::CanonicalXml::write (document) == after);
}

TEST_CASE ("new-cue lists: a cue is born with its settings, and one undo takes it back")
{
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\"><Cue id=\"B3N8R5TW\"/></List></Lists>"
        "<Mounts/><Audio tracks=\"2\"/></Show>", document).ok);
    const auto before = doc::CanonicalXml::write (document);

    struct Shape { std::string kind; doc::ShowDocument::Attributes attributes; };
    const std::vector<Shape> shapes {
        { "group", { { "mode", "timeline" } } },
        { "group", { { "mode", "sequence" }, { "advance", "manual" } } },
        { "group", { { "mode", "sequence" }, { "advance", "auto" } } },
        { "group", { { "mode", "sequence" }, { "advance", "auto" }, { "selection", "shuffle" } } },
        { "group", { { "mode", "sampler" } } },
        { "transport", { { "verb", "afterIteration" }, { "target", "B3N8R5TW" } } },
        { "start", { { "target", "B3N8R5TW" } } },
        { "midi", { { "type", "programChange" } } },
        { "mic", { { "onGo", "loop" } } },
    };

    std::int64_t tick = 0;

    for (const auto& shape : shapes)
    {
        CAPTURE (shape.kind);
        document.beginTransaction ("cue.create", tick, "window", {});
        tick += 100;

        const auto made = document.createCue ("7K2QM9X4", 1, shape.kind, "", {}, shape.attributes);
        REQUIRE (made.ok);

        for (const auto& [name, value] : shape.attributes)
            CHECK (document.getAttribute ("/godot/cue/" + made.id + "/" + name).value_or ("") == value);

        REQUIRE (document.undo (doc::UndoDomain::document).has_value());
        CHECK (doc::CanonicalXml::write (document) == before);
    }
}

TEST_CASE ("new-cue lists: a setting nobody could write afterwards is refused before an id is drawn")
{
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\"><Cue id=\"B3N8R5TW\"/></List></Lists>"
        "<Mounts/><Audio tracks=\"2\"/></Show>", document).ok);
    const auto before = doc::CanonicalXml::write (document);

    struct Refused { std::string kind; doc::ShowDocument::Attributes attributes; };
    const std::vector<Refused> refusals {
        { "transport", { { "nonsense", "1" } } },          // no such row
        { "transport", { { "verb", "sideways" } } },       // not one of its values
        { "transport", { { "kind", "memo" } } },           // derived from the element
        { "group", { { "order", "B3N8R5TW" } } },          // read-only
        { "memo", { { "name", "Rain" } } },                // has its own argument
        { "memo", { { "id", "B3N8R5TX" } } },              // so has this
    };

    for (const auto& refused : refusals)
    {
        CAPTURE (refused.attributes.front().first);
        CHECK_FALSE (document.createCue ("7K2QM9X4", 1, refused.kind, "", "T7AN5P0R", refused.attributes).ok);
        CHECK (doc::CanonicalXml::write (document) == before);
    }

    // Every refusal left the identifier free.
    CHECK (document.createCue ("7K2QM9X4", 1, "transport", "", "T7AN5P0R", { { "verb", "hard" } }).ok);
}

TEST_CASE ("new-cue lists: a group wrapped around cues is born with its settings, in one step")
{
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\"><Cue id=\"B3N8R5TW\"/><Cue id=\"P9XKC2WR\"/>"
        "<Cue id=\"F7HR8TVD\"/></List></Lists><Mounts/><Audio tracks=\"2\"/></Show>", document).ok);
    const auto before = doc::CanonicalXml::write (document);
    const std::string standby = "/godot/list/7K2QM9X4/standby";

    document.beginTransaction ("group.wrap", 0, "window", {});
    const auto shuffled = document.groupSelection ({ "P9XKC2WR", "B3N8R5TW" }, {},
                                                   { { "mode", "sequence" }, { "advance", "auto" },
                                                     { "selection", "shuffle" } });
    REQUIRE (shuffled.ok);
    CHECK (document.getAttribute ("/godot/cue/" + shuffled.id + "/advance").value_or ("") == "auto");
    CHECK (document.getAttribute ("/godot/cue/" + shuffled.id + "/selection").value_or ("") == "shuffle");
    CHECK (document.findById ("B3N8R5TW").getParent() == document.findById (shuffled.id));
    REQUIRE (document.undo (doc::UndoDomain::document).has_value());
    CHECK (doc::CanonicalXml::write (document) == before);

    /*  THE MODE IS THERE BEFORE THE CUES MOVE IN, so a pointer parked on a
        cue wrapped into a sampler lets go of it, as it would if the cue had
        been dragged into a sampler that already existed: a sampler's members
        are not places the pointer may stand. */
    REQUIRE (document.setAttribute (standby, "F7HR8TVD").ok);
    document.beginTransaction ("group.wrap", 100, "window", {});
    REQUIRE (document.groupSelection ({ "F7HR8TVD" }, {}, { { "mode", "sampler" } }).ok);
    CHECK (document.getAttribute (standby).value_or ("?").empty());

    // A bad setting refuses the whole wrap before anything moves.
    const auto wrapped = doc::CanonicalXml::write (document);
    CHECK_FALSE (document.groupSelection ({ "B3N8R5TW" }, {}, { { "mode", "sideways" } }).ok);
    CHECK (doc::CanonicalXml::write (document) == wrapped);
}

TEST_CASE ("import routing: explicit mono and stereo routes preserve assignments and undo")
{
    for (int channels : { 1, 2, 4 })
    {
        doc::ShowDocument document;
        REQUIRE (doc::CanonicalXml::read (
            "<Show><Lists><List id=\"7K2QM9X4\"><Media id=\"B3N8R5TW\" file=\"dropped.wav\"/></List></Lists><Mounts/>"
            "<Audio tracks=\"2\"><Bus id=\"P9XKC2WR\" firstChannel=\"4\" width=\"2\"/>"
            "<Bus id=\"J3MT5XYA\" width=\"2\"/></Audio></Show>", document).ok);
        const auto before = doc::CanonicalXml::write (document);
        document.beginTransaction ("route.default", 0, "test", {});
        const auto route = document.defaultMediaRoute ("B3N8R5TW", channels);
        REQUIRE (route.ok);
        const auto prefix = "/godot/route/" + route.id;
        CHECK (document.getAttribute (prefix + "/bus") == "J3MT5XYA");
        CHECK (document.getAttribute (prefix + "/gains") ==
               (channels == 1 ? "1 1" : channels == 2 ? "1 0 0 1" : "1 0 0 1 0 0 0 0"));
        const auto after = doc::CanonicalXml::write (document);
        REQUIRE (document.defaultMediaRoute ("B3N8R5TW", 1).ok);
        CHECK (doc::CanonicalXml::write (document) == after);
        CHECK (document.undo (doc::UndoDomain::document).has_value());
        CHECK (doc::CanonicalXml::write (document) == before);
        CHECK (document.redo (doc::UndoDomain::document).has_value());
        CHECK (doc::CanonicalXml::write (document) == after);
        doc::ShowDocument reopened;
        REQUIRE (doc::CanonicalXml::read (after, reopened).ok);
        CHECK (doc::CanonicalXml::write (reopened) == after);
    }
}

TEST_CASE ("import routing: missing bus and invalid channels leave the document unchanged")
{
    doc::ShowDocument document;
    REQUIRE (doc::CanonicalXml::read (
        "<Show><Lists><List id=\"7K2QM9X4\"><Media id=\"B3N8R5TW\"/></List></Lists><Mounts/><Audio tracks=\"2\"/></Show>", document).ok);
    const auto before = doc::CanonicalXml::write (document);
    CHECK_FALSE (document.defaultMediaRoute ("B3N8R5TW", 2).ok);
    CHECK_FALSE (document.defaultMediaRoute ("B3N8R5TW", 0).ok);
    CHECK_FALSE (document.defaultMediaRoute ("B3N8R5TW", 513).ok);
    CHECK (doc::CanonicalXml::write (document) == before);
}
