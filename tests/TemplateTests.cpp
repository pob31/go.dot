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
    A performance and its show's template (namespace draft §25, document/Template.h).

    Every case builds the same small world from the phase4 fixture - a show
    folder holding it as the template, and one performance copied from it -
    and edits the performance's show.xml as text, the way two files really
    come to differ: by somebody working in one of them.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/document/Template.h>
#include <wfg/client/model/TemplateReview.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace wfg;
namespace Template = wfg::doc::Template;

namespace
{
    struct Tour
    {
        Tour()
            : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                      .getChildFile ("wfg-template-" + juce::Uuid().toDashedString()))
        {
            const juce::File fixture { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/phase4" };
            REQUIRE (fixture.isDirectory());

            REQUIRE (show.createDirectory());
            REQUIRE (fixture.getChildFile ("show.xml").copyFileTo (show.getChildFile ("show.xml")));
            REQUIRE (show.getChildFile ("Hamlet.wfg").replaceWithText ("<Bundle formatVersion=\"1\"/>"));
            REQUIRE (fixture.getChildFile ("namespaces").copyDirectoryTo (show.getChildFile ("namespaces")));
            REQUIRE (show.getChildFile ("media").createDirectory());

            REQUIRE (performance.createDirectory());
            REQUIRE (show.getChildFile ("show.xml").copyFileTo (performance.getChildFile ("show.xml")));
            REQUIRE (performance.getChildFile ("Paris.wfg").replaceWithText ("<Bundle formatVersion=\"1\"/>"));
            REQUIRE (show.getChildFile ("namespaces").copyDirectoryTo (performance.getChildFile ("namespaces")));
            REQUIRE (performance.getChildFile ("media").createDirectory());
        }

        ~Tour() { root.deleteRecursively(); }

        //  The performance's show.xml, with `from` replaced by `to` once.
        void edit (const juce::String& from, const juce::String& to)
        {
            const auto file = performance.getChildFile ("show.xml");
            const auto text = file.loadFileAsString();
            REQUIRE_MESSAGE (text.contains (from), "the fixture no longer says " << from);
            REQUIRE (file.replaceWithText (text.replace (from, to)));
        }

        juce::String templateText() const { return show.getChildFile ("show.xml").loadFileAsString(); }

        const TemplateChange* find (const TemplateComparison& c, const std::string& id) const
        {
            const auto it = std::find_if (c.changes.begin(), c.changes.end(),
                                          [&id] (const TemplateChange& x) { return x.id == id; });
            return it == c.changes.end() ? nullptr : &*it;
        }

        juce::File root;
        juce::File show { root.getChildFile ("Hamlet") };
        juce::File performance { show.getChildFile ("Paris") };
    };

    bool hasField (const TemplateChange& change, const std::string& name)
    {
        return std::any_of (change.fields.begin(), change.fields.end(),
                            [&name] (const TemplateField& f) { return f.name == name; });
    }
}

TEST_CASE ("template: a performance copied from its template agrees with it")
{
    Tour tour;

    const auto comparison = Template::compare (tour.performance);
    REQUIRE_MESSAGE (comparison.ok, comparison.problem);
    CHECK (comparison.hasTemplate);
    CHECK (comparison.changes.empty());
}

TEST_CASE ("template: what a performance changed - added, changed by field, removed, settings")
{
    Tour tour;
    tour.edit ("level=\"-12\" name=\"After the scene\"", "level=\"-9\" name=\"After the scene\"");
    tour.edit ("<Fade id=\"P4FAD001\" duration=\"0.3\" level=\"-20\" name=\"Under the dialogue\" number=\"3\" target=\"P4MED003\"/>",
               "<Media id=\"NWC9E201\" file=\"announce.wav\" name=\"Announcement\" number=\"2.5\"/>");
    tour.edit ("name=\"Foldback\"", "name=\"Monitors\"");
    REQUIRE (tour.performance.getChildFile ("media").getChildFile ("announce.wav").replaceWithText ("x"));

    const auto comparison = Template::compare (tour.performance);
    REQUIRE_MESSAGE (comparison.ok, comparison.problem);

    const auto* changed = tour.find (comparison, "P4MED003");
    REQUIRE (changed != nullptr);
    CHECK (changed->kind == TemplateChange::Kind::changed);
    CHECK (hasField (*changed, "level"));
    CHECK_FALSE (hasField (*changed, "name"));

    const auto* added = tour.find (comparison, "NWC9E201");
    REQUIRE (added != nullptr);
    CHECK (added->kind == TemplateChange::Kind::added);
    CHECK (added->localSounds == std::vector<std::string> { "announce.wav" });

    const auto* removed = tour.find (comparison, "P4FAD001");
    REQUIRE (removed != nullptr);
    CHECK (removed->kind == TemplateChange::Kind::removed);

    const auto* settings = tour.find (comparison, "Audio");
    REQUIRE (settings != nullptr);
    CHECK (settings->kind == TemplateChange::Kind::settings);

    //  Nothing else: the persistent cues after the replaced one are not "moved".
    CHECK (comparison.changes.size() == 4);
}

TEST_CASE ("template: picks come in field by field, a new cue after its neighbour, and ids are kept")
{
    Tour tour;
    tour.edit ("level=\"-12\" name=\"After the scene\"", "level=\"-9\" name=\"After the night\"");
    tour.edit ("<Fade id=\"P4FAD001\"", "<Media id=\"NWC9E201\" name=\"Announcement\" number=\"2.5\"/><Fade id=\"P4FAD001\"");

    const auto updated = Template::update (tour.performance,
                                           { { "P4MED003", { "level" } }, { "NWC9E201", {} } }, false);
    REQUIRE_MESSAGE (updated.ok, updated.said);

    const auto text = tour.templateText();
    CHECK (text.contains ("level=\"-9\" name=\"After the scene\""));     // the level, not the name
    CHECK (text.indexOf ("NWC9E201") > text.indexOf ("P4MED003"));       // after its neighbour...
    CHECK (text.indexOf ("NWC9E201") < text.indexOf ("P4FAD001"));       // ...not at the end

    //  The same cue, so the next look finds only the field left behind.
    const auto again = Template::compare (tour.performance);
    REQUIRE (again.ok);
    REQUIRE (again.changes.size() == 1);
    CHECK (again.changes.front().id == "P4MED003");
    CHECK (hasField (again.changes.front(), "name"));

    //  And the template is still a show.
    doc::ShowDocument reopened;
    CHECK (doc::Bundle::open (tour.show, reopened).ok);
}

TEST_CASE ("template: a new cue whose neighbour the template lacks goes at the end of its list")
{
    Tour tour;
    tour.edit ("<Fade id=\"P4FAD001\"",
               "<Media id=\"NWC9E201\" name=\"First new\"/><Media id=\"NWC9E202\" name=\"Second new\"/><Fade id=\"P4FAD001\"");

    REQUIRE (Template::update (tour.performance, { { "NWC9E202", {} } }, false).ok);

    const auto text = tour.templateText();
    CHECK_FALSE (text.contains ("NWC9E201"));
    CHECK (text.indexOf ("NWC9E202") > text.indexOf ("P4PST001"));      // after the list's last item
    CHECK (text.indexOf ("NWC9E202") < text.indexOf ("P4FYR001"));      // and still in Act One
}

TEST_CASE ("template: a sound only the performance has is copied only when asked")
{
    for (const bool copy : { false, true })
    {
        Tour tour;
        tour.edit ("<Fade id=\"P4FAD001\"", "<Media id=\"NWC9E201\" file=\"announce.wav\" name=\"Announcement\"/><Fade id=\"P4FAD001\"");
        REQUIRE (tour.performance.getChildFile ("media").getChildFile ("announce.wav").replaceWithText ("x"));

        REQUIRE (Template::update (tour.performance, { { "NWC9E201", {} } }, copy).ok);
        CHECK (tour.show.getChildFile ("media").getChildFile ("announce.wav").existsAsFile() == copy);
        CHECK (tour.templateText().contains ("announce.wav"));
    }
}

TEST_CASE ("template: removed cues and settings come in only when picked")
{
    Tour tour;
    tour.edit ("<Fade id=\"P4FAD001\" duration=\"0.3\" level=\"-20\" name=\"Under the dialogue\" number=\"3\" target=\"P4MED003\"/>", "");
    tour.edit ("name=\"Foldback\"", "name=\"Monitors\"");

    REQUIRE (Template::update (tour.performance, {}, false).ok);
    CHECK (tour.templateText().contains ("P4FAD001"));
    CHECK (tour.templateText().contains ("Foldback"));

    REQUIRE (Template::update (tour.performance, { { "P4FAD001", {} }, { "Audio", {} } }, false).ok);
    CHECK_FALSE (tour.templateText().contains ("P4FAD001"));
    CHECK (tour.templateText().contains ("Monitors"));
    CHECK (Template::compare (tour.performance).changes.empty());
}

TEST_CASE ("template: a show with no template says so, and can be given one")
{
    Tour tour;
    REQUIRE (tour.show.getChildFile ("Hamlet.wfg").deleteFile());
    REQUIRE (tour.show.getChildFile ("show.xml").deleteFile());

    CHECK (Template::showFolderOf (tour.performance) == tour.show);      // media/ around is enough
    CHECK_FALSE (Template::hasTemplate (tour.show));

    const auto none = Template::compare (tour.performance);
    CHECK_FALSE (none.ok);
    CHECK_FALSE (none.hasTemplate);

    const auto made = Template::makeTemplate (tour.performance);
    REQUIRE_MESSAGE (made.ok, made.said);
    CHECK (Template::hasTemplate (tour.show));
    CHECK (Template::compare (tour.performance).changes.empty());
    CHECK_FALSE (Template::makeTemplate (tour.performance).ok);         // one is enough
}

TEST_CASE ("template: a document in a folder that is no show is no performance")
{
    Tour tour;
    const auto loose = tour.root.getChildFile ("Loose");
    REQUIRE (loose.createDirectory());

    CHECK (Template::showFolderOf (loose) == juce::File());
    CHECK_FALSE (Template::compare (loose).ok);
    CHECK_FALSE (Template::update (loose, { { "P4MED003", {} } }, false).ok);
}

TEST_CASE ("template review: added and changed ticked to start, removed and settings not; fields follow their cue")
{
    using wfg::client::model::TemplateReview;

    TemplateReview review ({
        { TemplateChange::Kind::added,    "NWC9E201", "2.5 Announcement", {}, { "announce.wav" } },
        { TemplateChange::Kind::changed,  "P4MED003", "2 After the scene",
          { { "level", "level" }, { "file", "file" } }, { "night.wav" } },
        { TemplateChange::Kind::removed,  "P4FAD001", "3 Under the dialogue", {}, {} },
        { TemplateChange::Kind::settings, "Audio",    "audio", {}, {} },
    });

    CHECK (review.isTicked (0));
    CHECK (review.isTicked (1));
    CHECK (review.isFieldTicked (1, 0));
    CHECK_FALSE (review.isTicked (2));
    CHECK_FALSE (review.isTicked (3));

    //  Unticking every field unticks the cue; one field back ticks it again.
    review.tickField (1, 0, false);
    review.tickField (1, 1, false);
    CHECK_FALSE (review.isTicked (1));
    review.tickField (1, 0, true);
    CHECK (review.isTicked (1));

    const auto picks = review.picks();
    REQUIRE (picks.size() == 2);
    CHECK (picks[0].id == "NWC9E201");
    CHECK (picks[1].id == "P4MED003");
    CHECK (picks[1].fields == std::vector<std::string> { "level" });

    //  The changed cue's sound comes only with its file ticked.
    CHECK (review.localSounds() == std::vector<std::string> { "announce.wav" });
    review.tickField (1, 1, true);
    CHECK (review.localSounds() == std::vector<std::string> { "announce.wav", "night.wav" });
}
