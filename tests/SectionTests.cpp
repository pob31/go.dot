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

/*  A SOUND CUE'S EDIT: ITS SECTIONS (namespace draft §55).

    A sound may be cut into sections - pieces of its file, each with an in and
    an out point, a trim and a crossfade at the join into it - and the sections
    in their order are the edited timeline the cue plays. This file is the
    document half of that: what a section IS, where it may sit, what it is
    published as, and the show a section makes impossible. The math of the
    timeline, the commands and the render have files of their own.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/SectionCommands.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

using namespace wfg;

namespace
{
    /*  A show with one sound cue and however many sections a case wants.

        The RangeRig's shape: a tree is published beside the document because
        `cue` and `index` are derived and refused storage, so the tree is the
        only place they can be read from. The sections are appended by hand,
        as a show read from disk holds them; the door that makes one is the
        commands' and has its own file. */
    struct SectionRig
    {
        SectionRig()
        {
            listId = document.createList ("Main").id;
            cueId = document.createCue (listId, 0, "media", "Rain").id;
        }

        std::string add (double in, double out, const std::string& id, const std::string& onCue = {})
        {
            juce::ValueTree section { "Section" };
            section.setProperty (juce::Identifier ("id"), juce::String (id), nullptr);
            section.setProperty (juce::Identifier ("in"), in, nullptr);
            section.setProperty (juce::Identifier ("out"), out, nullptr);
            document.findById (onCue.empty() ? cueId : onCue).appendChild (section, nullptr);
            return id;
        }

        /** A stored row, from the document. */
        std::string stored (const std::string& sectionId, const char* name) const
        {
            return document.getAttribute ("/godot/section/" + sectionId + "/" + name)
                     .value_or ("(absent)");
        }

        /** A published node, stored or derived, from the tree. */
        std::string published (const std::string& address)
        {
            parameters.markStale();

            tree::EngineState state;
            state.version = "test";

            const auto snapshot = parameters.publish (0, state);
            const auto* node = snapshot->find (address);

            if (node == nullptr || ! node->soleValue().has_value())
                return "(absent)";

            /*  A number spelled as the row would be read back: an index as
                "1", a crossfade as "0.01", in every locale. */
            return node->soleValue()->isString()
                     ? node->soleValue()->getString()
                     : osc::formatDouble (node->soleValue()->asDouble());
        }

        std::string published (const std::string& sectionId, const char* name)
        {
            return published ("/godot/section/" + sectionId + "/" + name);
        }

        static std::size_t saying (const std::vector<std::string>& sentences, const char* words)
        {
            return static_cast<std::size_t> (std::count_if (sentences.begin(), sentences.end(),
                                                            [words] (const std::string& sentence)
                                                            {
                                                                return sentence.find (words) != std::string::npos;
                                                            }));
        }

        Engine engine;
        doc::ShowDocument document;
        tree::MountTable mounts;
        cue::RunTable runs;
        tree::ParameterTree parameters { document, engine.commands(), mounts, runs };

        std::string listId, cueId;
    };
}

//==============================================================================
TEST_CASE ("section: it is published at an address of its own, with the cue and the position derived")
{
    /*  As a range is: flat, so a client watching one section watches one
        node, and positional, because the order of the sections IS the edited
        timeline. The index counts sections alone, so a range between two of
        them does not move the second. */
    SectionRig rig;

    const auto first = rig.add (0.0, 4.0, "SEC00001");
    REQUIRE (rig.document.createRange (rig.cueId, 1.0, 2.0).ok);
    const auto second = rig.add (10.0, 12.5, "SEC00002");

    CHECK (rig.published (first, "cue") == rig.cueId);
    CHECK (rig.published (second, "cue") == rig.cueId);
    CHECK (rig.published (first, "index") == "0");
    CHECK (rig.published (second, "index") == "1");

    CHECK (rig.stored (first, "in") == "0");
    CHECK (rig.stored (first, "out") == "4");
    CHECK (rig.stored (second, "in") == "10");
    CHECK (rig.stored (second, "out") == "12.5");

    /*  The rows a section gets by default: no trim, the click suppressor's
        crossfade. */
    CHECK (rig.published (first, "trim") == "0");
    CHECK (rig.published (first, "crossfade") == "0.01");

    /*  The cue lists them in order: the containment read back. */
    CHECK (rig.published ("/godot/cue/" + rig.cueId + "/sections") == "SEC00001 SEC00002");
}

TEST_CASE ("section: a cue with no sections lists none, and a memo lists nothing at all")
{
    SectionRig rig;

    CHECK (rig.published ("/godot/cue/" + rig.cueId + "/sections").empty());

    const auto memo = rig.document.createCue (rig.listId, 1, "memo", "House to half").id;
    CHECK (rig.published ("/godot/cue/" + memo + "/sections") == "(absent)");
}

TEST_CASE ("section: a section under a movie is refused when the show is read")
{
    /*  A child of Media alone (namespace draft §55): a movie's edit is a later
        round, and until then a Section under a Video is a show this build
        does not know how to play, refused as any unknown placement is. */
    SectionRig rig;

    const auto movie = rig.document.createCue (rig.listId, 1, "video", "Title").id;
    rig.add (0.0, 2.0, "SEC00003", movie);

    const auto xml = doc::CanonicalXml::write (rig.document);
    REQUIRE (xml.find ("<Section") != std::string::npos);

    doc::ShowDocument reopened;
    const auto result = doc::CanonicalXml::read (xml, reopened);

    CHECK_FALSE (result.ok);
    CHECK (SectionRig::saying (result.problems, "Section") >= 1);
}

TEST_CASE ("validate: a section that ends before it begins, or on a sound locked to a movie, is a problem")
{
    SectionRig rig;

    const auto id = rig.add (3.0, 3.0, "SEC00001");
    CHECK (SectionRig::saying (rig.document.validate(), "ends before it begins") == 1u);

    REQUIRE (rig.document.setAttribute ("/godot/section/" + id + "/out", "5").ok);
    CHECK (SectionRig::saying (rig.document.validate(), "ends before it begins") == 0);
    CHECK (rig.document.validate().empty());

    /*  A sound locked to a movie plays on the movie's time (WL): nothing of
        its own to cut up. */
    const auto movie = rig.document.createCue (rig.listId, 1, "video", "Title").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/lockedTo", movie).ok);
    CHECK (SectionRig::saying (rig.document.validate(), "locked to a movie") == 1u);

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/lockedTo", "").ok);
    CHECK (rig.document.validate().empty());
}

TEST_CASE ("validate: an edit source with no sections is a warning, never a refusal")
{
    /*  A frozen edit is its sections, kept for Unfreeze. A bounce a cue was
        pointed at by hand plays; it just has nothing to unfreeze to. */
    SectionRig rig;

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/editSource", "rain.wav").ok);
    CHECK (rig.document.validate().empty());
    CHECK (SectionRig::saying (rig.document.warnings(), "@editSource") == 1u);

    rig.add (0.0, 4.0, "SEC00001");
    CHECK (SectionRig::saying (rig.document.warnings(), "@editSource") == 0);
}

TEST_CASE ("section: it survives a save and a reload, being show state")
{
    /*  §4.10: a section is a decision - which piece of the recording, at what
        trim, joined how - so it is in the show file; `cue` and `index` are
        not, being facts the structure already carries. */
    SectionRig rig;

    const auto id = rig.add (12.0, 30.5, "SEC00001");
    REQUIRE (rig.document.setAttribute ("/godot/section/" + id + "/trim", "-6").ok);
    REQUIRE (rig.document.setAttribute ("/godot/section/" + id + "/crossfade", "0.25").ok);

    const auto xml = doc::CanonicalXml::write (rig.document);

    CHECK (xml.find ("<Section") != std::string::npos);
    CHECK (xml.find ("index=") == std::string::npos);

    doc::ShowDocument reopened;
    REQUIRE (doc::CanonicalXml::read (xml, reopened).ok);

    CHECK (reopened.getAttribute ("/godot/section/" + id + "/in").value_or ("?") == "12");
    CHECK (reopened.getAttribute ("/godot/section/" + id + "/out").value_or ("?") == "30.5");
    CHECK (reopened.getAttribute ("/godot/section/" + id + "/trim").value_or ("?") == "-6");
    CHECK (reopened.getAttribute ("/godot/section/" + id + "/crossfade").value_or ("?") == "0.25");
    CHECK (reopened.validate().empty());
}

//==============================================================================
/*  THE EDIT'S COMMANDS, AND THE CARRY (namespace draft §55, ADO).

    The methods are driven directly where the question is what they do to the
    document, and through the engine where the question is the record, the
    step or the road: `node.set`, `object.move` and `object.delete` must carry
    exactly as `section.*` does, since a client using the generic verbs must
    never leave a lane behind.
*/
namespace
{
    struct EditRig : SectionRig
    {
        EditRig()
        {
            REQUIRE (document.setAttribute ("/godot/cue/" + cueId + "/file", "rain.wav").ok);

            /*  The hook serve sets: one transaction per applied command, which
                is what makes a carry one undo step with the edit it followed. */
            engine.setBeforeApply ([this] (const Command& appliedCommand, const Event& submitted,
                                           const std::vector<osc::Value>& coerced, std::int64_t tickIndex)
                                   {
                                       document.beginTransaction (appliedCommand.name, tickIndex,
                                                                  submitted.origin, coerced);
                                   });

            doc::registerDocumentCommands (engine.commands(), document);
            doc::registerSectionCommands (engine.commands(), document,
                                          [] (const std::string& file) { return file == "rain.wav" ? 30.0 : 0.0; });
            engine.log().openInMemory ({});
        }

        /** One command through the engine; how many it applied. */
        std::size_t apply (const std::string& command, std::vector<osc::Value> args)
        {
            REQUIRE (engine.submit (origin::cli, command, std::move (args)));
            return engine.processTick (++tick).applied;
        }

        std::string split (double at)
        {
            const auto edit = document.splitSection (cueId, at, 30.0, {});
            REQUIRE (edit.ok);
            return edit.id;
        }

        std::vector<std::string> ids() const
        {
            std::vector<std::string> out;

            for (const auto& section : document.sectionsOf (document.findById (cueId)))
                out.push_back (section.id);

            return out;
        }

        std::string lane() const
        {
            return document.getAttribute ("/godot/cue/" + cueId + "/levelLane").value_or ("?");
        }

        double seconds (const std::string& address) const
        {
            return osc::parseDouble (document.getAttribute (address).value_or ("x")).value_or (-1.0);
        }

        std::int64_t tick = 0;
    };
}

TEST_CASE ("section.split: the first cut makes the whole file first, then cuts it; a cut, the top and the end divide nothing")
{
    EditRig rig;

    const auto second = rig.split (10.0);
    auto ids = rig.ids();

    REQUIRE (ids.size() == 2);
    CHECK (ids[1] == second);
    CHECK (doc::Id::isValid (ids[0]));
    CHECK (rig.stored (ids[0], "in") == "0");
    CHECK (rig.stored (ids[0], "out") == "10");
    CHECK (rig.stored (second, "in") == "10");
    CHECK (rig.stored (second, "out") == "30");

    CHECK (rig.document.splitSection (rig.cueId, 10.0, 30.0, {}).reason == "bad-value");
    CHECK (rig.document.splitSection (rig.cueId, 0.0, 30.0, {}).reason == "bad-value");
    CHECK (rig.document.splitSection (rig.cueId, 30.0, 30.0, {}).reason == "bad-value");
    CHECK (rig.document.splitSection (rig.cueId, 31.0, 30.0, {}).reason == "bad-value");

    /*  The second half lands directly after the piece it was cut from, with
        its trim. */
    REQUIRE (rig.document.setAttribute ("/godot/section/" + second + "/trim", "-6").ok);
    const auto third = rig.split (20.0);
    ids = rig.ids();

    REQUIRE (ids.size() == 3);
    CHECK (ids[1] == second);
    CHECK (ids[2] == third);
    CHECK (rig.stored (second, "out") == "20");
    CHECK (rig.stored (third, "in") == "20");
    CHECK (rig.stored (third, "trim") == "-6");

    /*  No length known and no section yet: nothing to make the whole from. */
    const auto other = rig.document.createCue (rig.listId, 1, "media", "Wind").id;
    CHECK (rig.document.splitSection (other, 1.0, 0.0, {}).reason == "bad-value");

    const auto memo = rig.document.createCue (rig.listId, 2, "memo", "Note").id;
    CHECK (rig.document.splitSection (memo, 1.0, 10.0, {}).reason == "type-mismatch");
    CHECK (rig.document.splitSection ("NOTACUE1", 1.0, 10.0, {}).reason == "unknown-id");
}

TEST_CASE ("section.split: the record carries the identifier drawn and the length the session knew")
{
    EditRig rig;

    CHECK (rig.apply ("section.split", { osc::Value::string (rig.cueId), osc::Value::float64 (12.0) }) == 1u);

    const auto parsed = LogFile::parse (rig.engine.log().contents());

    REQUIRE (parsed.records.size() == 1u);
    REQUIRE (parsed.records[0].kind == LogRecord::Kind::applied);
    REQUIRE (parsed.records[0].args.size() == 4);

    const auto ids = rig.ids();
    REQUIRE (ids.size() == 2);
    CHECK (parsed.records[0].args[2].getString() == ids[1]);
    CHECK (parsed.records[0].args[3].asDouble() == doctest::Approx (30.0));
    CHECK (rig.stored (ids[0], "out") == "12");
}

TEST_CASE ("section.join: only a cut that is still one in the file can be taken back")
{
    EditRig rig;

    const auto second = rig.split (10.0);
    const auto first = rig.ids()[0];

    CHECK (rig.document.joinSection (second).reason == "bad-value");   // nothing after it

    REQUIRE (rig.document.joinSection (first).ok);
    auto ids = rig.ids();
    REQUIRE (ids.size() == 1u);
    CHECK (ids[0] == first);
    CHECK (rig.stored (first, "out") == "30");
    CHECK_FALSE (rig.document.findById (second).isValid());

    /*  Parted by a move, the two are no longer one in the file. */
    const auto again = rig.split (10.0);
    REQUIRE (rig.document.moveSection (again, 0).ok);
    CHECK (rig.document.joinSection (again).reason == "bad-value");
}

TEST_CASE ("section.move: the lane, the ranges and the crossfade go with the sound, in one step")
{
    EditRig rig;

    rig.split (10.0);
    rig.split (20.0);
    const auto ids = rig.ids();   // the intro, the verse, the chorus
    REQUIRE (ids.size() == 3);

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/levelLane", "12 0 14 -20 16 -20 18 0").ok);
    const auto inVerse = rig.document.createRange (rig.cueId, 12.0, 18.0).id;
    const auto across = rig.document.createRange (rig.cueId, 5.0, 25.0).id;
    REQUIRE (rig.document.setAttribute ("/godot/section/" + ids[2] + "/crossfade", "0.5").ok);

    CHECK (rig.apply ("section.move", { osc::Value::string (ids[2]), osc::Value::int32 (0) }) == 1u);

    CHECK (rig.ids() == std::vector<std::string> { ids[2], ids[0], ids[1] });
    CHECK (rig.lane() == "22 0 24 -20 26 -20 28 0");
    CHECK (rig.seconds ("/godot/range/" + inVerse + "/in") == doctest::Approx (22.0));
    CHECK (rig.seconds ("/godot/range/" + inVerse + "/out") == doctest::Approx (28.0));
    CHECK_FALSE (rig.document.findById (across).isValid());   // the two sides of the move

    /*  The intro now follows the chorus, and its in point is the file's
        start: nothing before it for a crossfade, so its join is a hard cut.
        The chorus's own crossfade is kept, unheard at the front. */
    CHECK (rig.seconds ("/godot/section/" + ids[0] + "/crossfade") == doctest::Approx (0.0));
    CHECK (rig.seconds ("/godot/section/" + ids[2] + "/crossfade") == doctest::Approx (0.5));

    /*  One undo puts all of it back. */
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.ids() == ids);
    CHECK (rig.lane() == "12 0 14 -20 16 -20 18 0");
    CHECK (rig.document.findById (across).isValid());
    CHECK (rig.seconds ("/godot/range/" + inVerse + "/in") == doctest::Approx (12.0));
    CHECK (rig.seconds ("/godot/section/" + ids[0] + "/crossfade") == doctest::Approx (0.01));
}

TEST_CASE ("section.move: the start offset goes with the sound, and nought stays nought")
{
    EditRig rig;

    rig.split (10.0);
    rig.split (20.0);
    const auto ids = rig.ids();

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/startOffset", "22").ok);
    REQUIRE (rig.document.moveSection (ids[2], 0).ok);
    CHECK (rig.seconds ("/godot/cue/" + rig.cueId + "/startOffset") == doctest::Approx (2.0));

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/startOffset", "0").ok);
    REQUIRE (rig.document.moveSection (ids[2], 2).ok);
    CHECK (rig.seconds ("/godot/cue/" + rig.cueId + "/startOffset") == doctest::Approx (0.0));
    CHECK (rig.ids() == ids);
}

TEST_CASE ("section.remove: what sat on the removed material goes with it, and the last one gone is the file again")
{
    EditRig rig;

    rig.split (10.0);
    rig.split (20.0);
    const auto ids = rig.ids();

    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/levelLane", "5 0 12 -20 18 -20 25 0").ok);
    const auto inVerse = rig.document.createRange (rig.cueId, 12.0, 18.0).id;
    const auto across = rig.document.createRange (rig.cueId, 5.0, 25.0).id;

    REQUIRE (rig.document.removeSection (ids[1]).ok);
    CHECK (rig.ids() == std::vector<std::string> { ids[0], ids[2] });
    CHECK (rig.lane() == "5 0 15 0");
    CHECK_FALSE (rig.document.findById (inVerse).isValid());
    CHECK (rig.seconds ("/godot/range/" + across + "/in") == doctest::Approx (5.0));
    CHECK (rig.seconds ("/godot/range/" + across + "/out") == doctest::Approx (15.0));

    /*  The intro gone too: the chorus is the whole edit, its point at five,
        the range what was left of it over the chorus. */
    REQUIRE (rig.document.removeSection (ids[0]).ok);
    CHECK (rig.lane() == "5 0");
    CHECK (rig.seconds ("/godot/range/" + across + "/in") == doctest::Approx (0.0));
    CHECK (rig.seconds ("/godot/range/" + across + "/out") == doctest::Approx (5.0));

    /*  The last one gone: the cue plays its file, and the points are carried
        back to the file's own time - the chorus's point to the chorus. */
    REQUIRE (rig.document.removeSection (ids[2]).ok);
    CHECK (rig.ids().empty());
    CHECK (rig.lane() == "25 0");
    CHECK (rig.seconds ("/godot/range/" + across + "/in") == doctest::Approx (20.0));
    CHECK (rig.seconds ("/godot/range/" + across + "/out") == doctest::Approx (25.0));
}

TEST_CASE ("section.clear: everything carried back to the file's own time")
{
    EditRig rig;

    rig.split (10.0);
    rig.split (20.0);
    const auto ids = rig.ids();
    REQUIRE (rig.document.moveSection (ids[2], 0).ok);
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/levelLane", "2 -6 12 0").ok);

    CHECK (rig.document.clearSections (rig.cueId).ok);
    CHECK (rig.ids().empty());
    CHECK (rig.lane() == "2 0 22 -6");
    CHECK (rig.document.clearSections (rig.cueId).reason == "bad-value");
}

TEST_CASE ("section: every road carries - node.set on an edge, object.move and object.delete")
{
    EditRig rig;

    rig.split (10.0);
    rig.split (20.0);
    const auto ids = rig.ids();
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/levelLane", "5 0 9 -3 15 -6").ok);

    /*  An out point written as a node is an edge moved: the point on the
        sliver cut away goes, the rest shifts. And the record is the one
        node.set that was sent. */
    CHECK (rig.apply ("node.set", { osc::Value::string ("/godot/section/" + ids[0] + "/out"), osc::Value::string ("8") }) == 1u);
    CHECK (rig.stored (ids[0], "out") == "8");
    CHECK (rig.lane() == "5 0 13 -6");

    const auto parsed = LogFile::parse (rig.engine.log().contents());
    REQUIRE (parsed.records.size() == 1u);
    CHECK (parsed.records[0].command == "node.set");

    /*  The generic move and delete carry as the section verbs do. */
    REQUIRE (rig.document.move (ids[2], rig.cueId, 0).ok);
    CHECK (rig.ids() == std::vector<std::string> { ids[2], ids[0], ids[1] });
    CHECK (rig.lane() == "15 0 23 -6");

    REQUIRE (rig.document.remove (ids[2]).ok);
    CHECK (rig.ids() == std::vector<std::string> { ids[0], ids[1] });
    CHECK (rig.lane() == "5 0 13 -6");

    /*  Into another cue a section goes nowhere. */
    const auto other = rig.document.createCue (rig.listId, 1, "media", "Wind").id;
    CHECK (rig.document.move (ids[0], other, 0).reason == "bad-address");
}

TEST_CASE ("section: a crossfade written is held to the material, and a trim is a plain write")
{
    EditRig rig;

    rig.split (10.0);
    const auto ids = rig.ids();

    /*  Up to twice the in point. */
    REQUIRE (rig.document.setAttribute ("/godot/section/" + ids[1] + "/crossfade", "25").ok);
    CHECK (rig.seconds ("/godot/section/" + ids[1] + "/crossfade") == doctest::Approx (20.0));
    CHECK (rig.document.setAttribute ("/godot/section/" + ids[1] + "/crossfade", "-1").reason == "bad-value");
    CHECK (rig.document.setAttribute ("/godot/section/" + ids[1] + "/crossfade", "soon").reason == "type-mismatch");

    REQUIRE (rig.document.setAttribute ("/godot/section/" + ids[1] + "/trim", "-4.5").ok);
    CHECK (rig.stored (ids[1], "trim") == "-4.5");
    CHECK (rig.document.setAttribute ("/godot/section/" + ids[1] + "/trim", "13").reason == "type-mismatch");   // the row's range, as every row's
}

TEST_CASE ("media.frozen and media.unfreeze: the file swapped, the sections kept, every edit refused in between")
{
    EditRig rig;

    rig.split (10.0);
    const auto ids = rig.ids();
    const auto base = "/godot/cue/" + rig.cueId + "/";

    /*  Only an open edit, onto the file it was made from. */
    CHECK (rig.document.freezeEdit (rig.cueId, "wind.wav", "rain (edit).wav").reason == "bad-value");
    CHECK (rig.document.unfreezeEdit (rig.cueId).reason == "bad-value");

    CHECK (rig.apply ("media.frozen", { osc::Value::string (rig.cueId), osc::Value::string ("rain.wav"),
                                        osc::Value::string ("rain (edit).wav") }) == 1u);

    CHECK (rig.document.getAttribute (base + "file").value_or ("?") == "rain (edit).wav");
    CHECK (rig.document.getAttribute (base + "editSource").value_or ("?") == "rain.wav");
    CHECK (rig.ids() == ids);
    CHECK (rig.document.isFrozenEdit (rig.document.findById (rig.cueId)));
    CHECK_FALSE (rig.document.hasOpenEdit (rig.document.findById (rig.cueId)));

    CHECK (rig.document.splitSection (rig.cueId, 5.0, 30.0, {}).reason == "frozen");
    CHECK (rig.document.moveSection (ids[1], 0).reason == "frozen");
    CHECK (rig.document.removeSection (ids[1]).reason == "frozen");
    CHECK (rig.document.trimSection (ids[1], 10.0, 20.0).reason == "frozen");
    CHECK (rig.document.joinSection (ids[0]).reason == "frozen");
    CHECK (rig.document.clearSections (rig.cueId).reason == "frozen");
    CHECK (rig.document.setAttribute ("/godot/section/" + ids[1] + "/trim", "-3").reason == "frozen");
    CHECK (rig.document.setAttribute ("/godot/section/" + ids[1] + "/in", "11").reason == "frozen");
    CHECK (rig.document.freezeEdit (rig.cueId, "rain (edit).wav", "x.wav").reason == "bad-value");

    /*  Undone, the swap is undone whole. */
    REQUIRE (rig.document.undo (doc::UndoDomain::document).has_value());
    CHECK (rig.document.getAttribute (base + "file").value_or ("?") == "rain.wav");
    CHECK (rig.document.getAttribute (base + "editSource").value_or ("?").empty());
    CHECK (rig.document.hasOpenEdit (rig.document.findById (rig.cueId)));

    REQUIRE (rig.document.freezeEdit (rig.cueId, "rain.wav", "rain (edit).wav").ok);
    CHECK (rig.apply ("media.unfreeze", { osc::Value::string (rig.cueId) }) == 1u);
    CHECK (rig.document.getAttribute (base + "file").value_or ("?") == "rain.wav");
    CHECK (rig.document.getAttribute (base + "editSource").value_or ("?").empty());
    CHECK (rig.ids() == ids);
    REQUIRE (rig.document.splitSection (rig.cueId, 5.0, 30.0, {}).ok);
}

TEST_CASE ("section: a sound locked to a movie and a locked show refuse every section edit")
{
    EditRig rig;

    rig.split (10.0);
    const auto ids = rig.ids();

    const auto movie = rig.document.createCue (rig.listId, 1, "video", "Title").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/lockedTo", movie).ok);
    CHECK (rig.document.splitSection (rig.cueId, 5.0, 30.0, {}).reason == "locked-to-movie");
    CHECK (rig.document.moveSection (ids[1], 0).reason == "locked-to-movie");
    CHECK (rig.document.freezeEdit (rig.cueId, "rain.wav", "rain (edit).wav").reason == "locked-to-movie");
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/lockedTo", "").ok);

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (rig.document.splitSection (rig.cueId, 5.0, 30.0, {}).reason == "locked");
    CHECK (rig.document.removeSection (ids[1]).reason == "locked");
    CHECK (rig.document.setAttribute ("/godot/section/" + ids[1] + "/in", "11").reason == "locked");
    CHECK (rig.document.freezeEdit (rig.cueId, "rain.wav", "rain (edit).wav").reason == "locked");
    CHECK (rig.document.unfreezeEdit (rig.cueId).reason == "locked");
}
