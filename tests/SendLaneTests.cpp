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
    A send's lane, in the document (namespace draft §28): the write door and
    the loader judge it with the level lane's own function (PX), and a mic
    cue's send, which has no file to follow, takes none (QA).

    What a lane asks for at a second of the file is `LevelLaneTests`', being
    the same arithmetic; the Runner's offset is asked in `GoTests`, beside the
    other things that decide a send. A serialisation surface, so every case
    here runs under fr_FR as well as C.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/command/Command.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/ShowDocument.h>

#include <string>

using namespace wfg;

namespace
{
    /*  A media cue sending into a stereo mix, and a mic cue sending into the
        same one. Hand-written, as every fixture of the document's is. */
    std::string showWith (const std::string& mediaSend, const std::string& micSend)
    {
        return "<Show>\n"
               "  <Lists>\n"
               "    <List id=\"SN000001\" name=\"Sound\">\n"
               "      <Media id=\"SN000002\" file=\"rain.wav\" name=\"Rain\">\n"
               "        <Send id=\"SN000003\" bus=\"SN000010\"" + mediaSend + "/>\n"
               "      </Media>\n"
               "      <Mic id=\"SN000004\" channel=\"SN000020\" input=\"SN000030\" name=\"Voix\">\n"
               "        <Send id=\"SN000005\" bus=\"SN000010\"" + micSend + "/>\n"
               "      </Mic>\n"
               "    </List>\n"
               "  </Lists>\n"
               "  <Mounts/>\n"
               "  <Audio tracks=\"2\">\n"
               "    <Bus id=\"SN000010\" kind=\"mix\" name=\"Loin\" width=\"2\"/>\n"
               "    <Inputs>\n"
               "      <Input id=\"SN000030\" name=\"Voix\"/>\n"
               "    </Inputs>\n"
               "    <Rack>\n"
               "      <Channel id=\"SN000020\" class=\"monoToStereo\" name=\"Vox 1\"/>\n"
               "    </Rack>\n"
               "  </Audio>\n"
               "</Show>\n";
    }

    doc::ShowDocument opened (const std::string& xml)
    {
        doc::ShowDocument document;
        const auto result = doc::CanonicalXml::read (xml, document);

        for (const auto& problem : result.problems)
            INFO (problem);

        REQUIRE (result.ok);
        return document;
    }
}

TEST_CASE ("send lane: a media cue's send takes a lane through the door, and a list that is not one is refused")
{
    auto document = opened (showWith ("", ""));
    const std::string lane = "/godot/send/SN000003/levelLane";

    /*  None drawn: the default, and the send plays as written. */
    CHECK (document.getAttribute (lane) == std::string (""));

    /*  The sound travels away and comes back: down to the distant speakers'
        silence over two seconds, held, and up again. */
    REQUIRE (document.setAttribute (lane, "10 0  12.50 -40 30 -40 32 0").ok);
    CHECK (document.getAttribute (lane) == std::string ("10 0 12.5 -40 30 -40 32 0"));
    CHECK (document.validate().empty());

    /*  The level lane's refusals, every one: it is the same judge (PX). */
    struct Case { const char* text; const char* refusal; const char* why; };

    const Case cases[] = {
        { "10 0 12",            reason::badValue,     "an odd count - a point is a second and a level" },
        { "10 0 10 -6",         reason::badValue,     "two points at one second are a jump" },
        { "10 0 8 -6",          reason::badValue,     "seconds that go back" },
        { "-1 0 4 -6",          reason::badValue,     "a second before the file starts" },
        { "4 -130",             reason::badValue,     "a level no send may be at" },
        { "4 13",               reason::badValue,     "a level above a send's 12 dB" },
        { "4 0 later -12",      reason::typeMismatch, "an element that is not a number at all" },
    };

    for (const auto& c : cases)
    {
        INFO (c.why << ": \"" << c.text << "\"");
        CHECK (document.setAttribute (lane, c.text).reason == std::string (c.refusal));

        /*  And a refusal leaves the lane that was there. */
        CHECK (document.getAttribute (lane) == std::string ("10 0 12.5 -40 30 -40 32 0"));
    }

    /*  Written into the file on the send, and cleared, the file says nothing. */
    CHECK (doc::CanonicalXml::write (document).find ("levelLane=\"10 0 12.5 -40 30 -40 32 0\"")
             != std::string::npos);

    REQUIRE (document.setAttribute (lane, "").ok);
    CHECK (doc::CanonicalXml::write (document).find ("levelLane=") == std::string::npos);
}

TEST_CASE ("send lane: a mic cue's send has no file to follow, and takes no lane")
{
    auto document = opened (showWith ("", ""));
    const std::string lane = "/godot/send/SN000005/levelLane";

    /*  QA, CX's reason: a lane is bound to a file's clock, and a live input
        has none. Refused rather than kept and ignored, which would be a curve
        on the screen that nobody hears. */
    CHECK (document.setAttribute (lane, "0 0 4 -12").reason == std::string (reason::badValue));
    CHECK (document.getAttribute (lane) == std::string (""));

    /*  No lane is no lane, on a mic cue's send as on any other. */
    CHECK (document.setAttribute (lane, "").ok);
}

TEST_CASE ("send lane: a file holding a send lane that is not one does not open, and says why")
{
    struct Case { const char* media; const char* mic; const char* mentions; };

    const Case cases[] = {
        { " levelLane=\"4 0 5.5\"",       "",                            "odd number" },
        { " levelLane=\"4 0 3 -6\"",      "",                            "does not come after" },
        { " levelLane=\"4 -200\"",        "",                            "outside -120..12" },
        { "",                             " levelLane=\"0 0 4 -12\"",    "plays no file" },
    };

    for (const auto& c : cases)
    {
        INFO ("media send" << c.media << ", mic send" << c.mic);

        doc::ShowDocument document;
        const auto result = doc::CanonicalXml::read (showWith (c.media, c.mic), document);

        CHECK_FALSE (result.ok);

        bool mentioned = false;
        std::string reported;

        for (const auto& problem : result.problems)
        {
            reported += "\n  " + problem;

            if (problem.find ("levelLane") != std::string::npos
                && problem.find (c.mentions) != std::string::npos)
                mentioned = true;
        }

        INFO ("reported:" << reported);
        CHECK (mentioned);
    }

    /*  And a good one opens, and is the lane it was. */
    auto document = opened (showWith (" levelLane=\"0 -6 4 0\"", ""));
    CHECK (document.getAttribute ("/godot/send/SN000003/levelLane") == std::string ("0 -6 4 0"));
}
