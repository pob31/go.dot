/* Go.dot — Copyright (C) 2026 Pierre-Olivier Boulant
   SPDX-License-Identifier: GPL-3.0-or-later */
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/document/CanonicalXml.h>

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
