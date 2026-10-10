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
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/ParameterTree.h>

#include <algorithm>
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
    CHECK (SectionRig::saying (rig.document.validate(), "ends before it begins") == 1);

    REQUIRE (rig.document.setAttribute ("/godot/section/" + id + "/out", "5").ok);
    CHECK (SectionRig::saying (rig.document.validate(), "ends before it begins") == 0);
    CHECK (rig.document.validate().empty());

    /*  A sound locked to a movie plays on the movie's time (WL): nothing of
        its own to cut up. */
    const auto movie = rig.document.createCue (rig.listId, 1, "video", "Title").id;
    REQUIRE (rig.document.setAttribute ("/godot/cue/" + rig.cueId + "/lockedTo", movie).ok);
    CHECK (SectionRig::saying (rig.document.validate(), "locked to a movie") == 1);

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
    CHECK (SectionRig::saying (rig.document.warnings(), "@editSource") == 1);

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
