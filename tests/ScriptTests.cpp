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
    Script cues (namespace draft §66): a processor written as text, in
    JavaScript or Lua, run as a process cue's patch is run.

    In stages: the kind in the document (SC.2), the JavaScript instance alone
    (SC.3), the cue on the Runner (SC.4). A serialisation surface and a
    language that reads numbers, so every case runs under fr_FR as well as C.
*/
#include <3rd_party/doctest/tracktion_doctest.hpp>
#include "TestSupport.h"
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/ShowDocument.h>

#include <string>

using namespace wfg;
using namespace wfg::doc;

//==============================================================================
/*  SC.2: THE KIND IN THE DOCUMENT (AMM). */

TEST_CASE ("script cue: made with its language, its rows read back, and an unknown language refused")
{
    ShowDocument document;
    const auto listId = document.createList ("Main").id;

    const auto made = document.createCue (listId, 0, "script", "Doubler", {},
                                          { { "language", "lua" }, { "source", "godot.send('/a', 1)" } });
    REQUIRE (made.ok);

    const auto cue = document.findById (made.id);
    CHECK (cue.getType().toString() == "Script");

    const auto base = "/godot/cue/" + made.id + "/";
    CHECK (document.getAttribute (base + "language") == std::optional<std::string> ("lua"));
    CHECK (document.getAttribute (base + "source") == std::optional<std::string> ("godot.send('/a', 1)"));
    CHECK (document.getAttribute (base + "midiIn") == std::optional<std::string> (""));
    CHECK (document.getAttribute (base + "midiOut") == std::optional<std::string> (""));

    //  Born with nothing: JavaScript, and a script that does nothing.
    const auto plain = document.createCue (listId, 1, "script", {});
    REQUIRE (plain.ok);
    CHECK (document.getAttribute ("/godot/cue/" + plain.id + "/language") == std::optional<std::string> ("javascript"));
    CHECK (document.getAttribute ("/godot/cue/" + plain.id + "/source") == std::optional<std::string> (""));

    //  A language Go.dot does not run is refused, as an enum is.
    CHECK_FALSE (document.setAttribute ("/godot/cue/" + plain.id + "/language", "python").ok);
    CHECK (document.setAttribute ("/godot/cue/" + plain.id + "/language", "lua").ok);

    //  A script cue carries no patch, and a process cue no source.
    const auto& schema = Schema::instance();
    CHECK (schema.attribute ("Script", "patch") == nullptr);
    CHECK (schema.attribute ("Process", "source") == nullptr);
    CHECK (schema.attribute ("Script", "source") != nullptr);

    CHECK (document.validate().empty());
}

TEST_CASE ("script cue: written and read back as it was, its source's lines and quotes whole")
{
    ShowDocument document;
    const auto listId = document.createList ("Main").id;

    const std::string source = "// Doubles what it hears.\n"
                               "godot.hear('/wfs/source/1/x', (x) => {\n"
                               "    godot.send(\"/wfs/source/2/x\", x * 2.5);\n"
                               "});\n";
    const auto made = document.createCue (listId, 0, "script", "Doubler", {},
                                          { { "language", "javascript" }, { "source", source } });
    REQUIRE (made.ok);

    const auto written = CanonicalXml::write (document);
    CHECK (written.find ("<Script ") != std::string::npos);

    ShowDocument again;
    const auto read = CanonicalXml::read (written, again);
    const auto problem = read.problems.empty() ? std::string {} : read.problems.front();
    INFO (problem);
    REQUIRE (read.ok);
    CHECK (again.getAttribute ("/godot/cue/" + made.id + "/source") == std::optional<std::string> (source));
    CHECK (CanonicalXml::write (again) == written);
}

TEST_CASE ("script cue: a time group and a sampler take no script cue as a member, as they take no patch")
{
    ShowDocument document;
    const auto listId = document.createList ("Main").id;
    const auto group = document.createCue (listId, 0, "group", "Bank", {}, { { "mode", "sampler" } });
    REQUIRE (group.ok);

    const auto made = document.createCue (group.id, 0, "script", {});
    CHECK_FALSE (made.ok);
    CHECK (document.findById (group.id).getNumChildren() == 0);
}
