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
    Process cues (namespace draft 51): Pd's text, and a patch run on a thread of
    its own.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/process/PatchText.h>
#include <wfg/engine/process/PdInstance.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

using namespace wfg::process;
using namespace std::chrono_literals;

namespace
{
    /*  A patch made in Pd 0.56: a subpatch, a message box with escapes, a
        number box with names, a box given a width, a comment with a comma. */
    const char* const madeInPd =
        "#N canvas 312 140 527 410 12;\n"
        "#X obj 40 40 r /wfs/source/1/x;\n"
        "#X obj 40 80 * 2.5;\n"
        "#X obj 40 120 s /wfs/source/2/x;\n"
        "#X msg 200 40 \\; /godot/cmd/cue/fire F7HR8TVD \\; local 1;\n"
        "#X floatatom 200 120 5 0 0 0 - /godot/dca/D1/trim /out/level 0;\n"
        "#N canvas 0 50 450 300 inner 0;\n"
        "#X obj 20 20 inlet;\n"
        "#X obj 20 60 outlet;\n"
        "#X connect 0 0 1 0;\n"
        "#X restore 40 200 pd inner;\n"
        "#X obj 40 160 metro 100, f 20;\n"
        "#X text 200 200 a comment\\, with a comma \\$1;\n"
        "#X obj 300 300 tgl 15 0 /tgl/out /tgl/in empty 17 7 0 10 #fcfcfc #000000 #000000 0 1;\n"
        "#X connect 0 0 1 0;\n"
        "#X connect 1 0 2 0;\n"
        "#X connect 5 0 2 0;\n";

    std::filesystem::path scratch (const std::string& name)
    {
        return std::filesystem::temp_directory_path() / "wfg-process-tests" / name;
    }

    PdInstance::Settings settingsFor (const std::string& name)
    {
        PdInstance::Settings settings;
        settings.patchFile = (scratch (name) / (name + ".pd")).string();
        return settings;
    }

    /*  Make and open, each waited for: a test with one instance is a quiet point. */
    void makeAndOpen (PdInstance& pd, const std::string& text)
    {
        REQUIRE (pd.make());
        REQUIRE (pd.finished (5s));
        REQUIRE (pd.made());
        REQUIRE (pd.open (text));
        REQUIRE (pd.finished (5s));
        INFO (pd.problem());
        REQUIRE (pd.opened());
    }

    Outbox tickOnce (PdInstance& pd, std::vector<Input> inputs = {})
    {
        REQUIRE (pd.tick (std::move (inputs)));
        REQUIRE (pd.finished (5s));
        return pd.takeOutbox();
    }

    std::vector<double> numbersSentTo (const Outbox& outbox, const std::string& to)
    {
        std::vector<double> out;
        for (const auto& sent : outbox.sent)
            if (sent.to == to)
                for (const auto& atom : sent.atoms)
                    if (atom.isNumber)
                        out.push_back (atom.number);
        return out;
    }

    bool sameNumber (double a, double b)
    {
        return std::abs (a - b) < 1e-6;
    }
}

TEST_CASE ("process: Pd's text is read and written back byte for byte")
{
    const auto patch = parsePatch (madeInPd);
    CHECK (patch.problem.empty());
    CHECK (writePatch (patch) == madeInPd);

    REQUIRE (patch.canvases.size() == 2);
    CHECK_FALSE (patch.canvases[0].parent.has_value());
    CHECK (patch.canvases[1].parent == std::optional<std::size_t> (0));
    CHECK (patch.canvases[1].name == "pd inner");

    const auto top = patch.boxesOn (0);
    REQUIRE (top.size() == 9);
    CHECK (patch.boxes[top[0]].text == "r /wfs/source/1/x");
    CHECK (patch.boxes[top[0]].x == 40);
    CHECK (patch.boxes[top[0]].y == 40);
    CHECK (patch.boxes[top[3]].kind == BoxKind::message);
    CHECK (patch.boxes[top[4]].kind == BoxKind::number);
    CHECK (patch.boxes[top[5]].kind == BoxKind::subpatch);
    CHECK (patch.boxes[top[5]].text == "pd inner");
    CHECK (patch.boxes[top[6]].text == "metro 100");
    CHECK (patch.boxes[top[6]].width == 20);
    CHECK (patch.boxes[top[7]].kind == BoxKind::comment);
    CHECK (unescaped (patch.boxes[top[7]].text) == "a comment, with a comma $1");

    // The subpatch's own boxes are numbered from nought on their canvas.
    CHECK (patch.boxesOn (1).size() == 2);

    REQUIRE (patch.lines.size() == 4);
    CHECK (patch.lines[0].canvas == 1);
    CHECK (patch.lines[3].canvas == 0);
    CHECK (patch.lines[3].fromBox == 5);
    CHECK (patch.lines[3].toBox == 2);
}

TEST_CASE ("process: a patch saved with Windows line endings is read and written back the same")
{
    std::string crlf;
    for (const char c : std::string (madeInPd))
    {
        if (c == '\n')
            crlf += '\r';
        crlf += c;
    }
    const auto patch = parsePatch (crlf);
    CHECK (patch.problem.empty());
    CHECK (patch.boxesOn (0).size() == 9);
    CHECK (writePatch (patch) == crlf);
}

TEST_CASE ("process: what is not a patch says why and is still written back")
{
    CHECK (parsePatch ("hello there").problem == "the text holds no canvas");
    CHECK (writePatch (parsePatch ("hello there")) == "hello there");
    CHECK (parsePatch ("#N canvas 0 0 450 300 12;\n#N canvas 0 0 10 10 sub 0;\n").problem
           == "a subpatch is opened and never closed");
    CHECK (parsePatch ("").problem.empty());
}

TEST_CASE ("process: the names a patch sends to and hears that Go.dot answers")
{
    const auto names = namesIn (parsePatch (madeInPd));
    CHECK (names.sends == std::vector<std::string> { "/wfs/source/2/x", "/godot/cmd/cue/fire", "/out/level", "/tgl/out" });
    CHECK (names.receives == std::vector<std::string> { "/wfs/source/1/x", "/godot/dca/D1/trim", "/tgl/in" });

    const auto catchAll = namesIn (parsePatch ("#N canvas 0 0 450 300 12;\n#X obj 10 10 r in;\n#X obj 10 40 s out;\n"
                                               "#X obj 10 70 s local;\n"));
    CHECK (catchAll.sends == std::vector<std::string> { "out" });
    CHECK (catchAll.receives == std::vector<std::string> { "in" });
}

TEST_CASE ("process: a record made here is written as Pd writes one, a comma after its word")
{
    const auto record = recordOf ({ "#X", "obj", "10", "20", "metro", "100", ",", "f", "20" });
    CHECK (record.raw == "#X obj 10 20 metro 100, f 20;\n");
    CHECK (escaped ("a b;c,d$1\\") == "a\\ b\\;c\\,d\\$1\\\\");
    CHECK (unescaped (escaped ("a b;c,d$1\\")) == "a b;c,d$1\\");
    CHECK (splitWords ("a\\ b c, d") == std::vector<std::string> { "a\\ b", "c", ",", "d" });
}

TEST_CASE ("process: a new cue's patch opens empty but for its comment")
{
    const auto patch = parsePatch (starterPatch());
    CHECK (patch.problem.empty());
    REQUIRE (patch.boxes.size() == 1);
    CHECK (patch.boxes[0].kind == BoxKind::comment);
    CHECK (namesIn (patch).sends.empty());
}

TEST_CASE ("process: a patch doubles what it hears, on a thread of its own")
{
    PdInstance pd (settingsFor ("double"));
    makeAndOpen (pd, "#N canvas 0 0 450 300 12;\n"
                     "#X obj 10 10 r /a;\n"
                     "#X obj 10 40 * 2;\n"
                     "#X obj 10 70 s /b;\n"
                     "#X connect 0 0 1 0;\n"
                     "#X connect 1 0 2 0;\n");
    CHECK (pd.sends() == std::vector<std::string> { "/b" });
    CHECK (pd.receives() == std::vector<std::string> { "/a" });

    const auto outbox = tickOnce (pd, { { "/a", { Atom::of (21.0) } } });
    const auto doubled = numbersSentTo (outbox, "/b");
    REQUIRE (doubled.size() == 1);
    CHECK (sameNumber (doubled[0], 42.0));

    // Nothing heard, nothing sent; and a name the patch does not hear is passed by.
    CHECK (tickOnce (pd).sent.empty());
    CHECK (tickOnce (pd, { { "/nobody", { Atom::of (1.0) } } }).sent.empty());
}

TEST_CASE ("process: Pd's clock is the tick - a metro of 100 ms bangs every fifth tick, ten a second")
{
    PdInstance pd (settingsFor ("metro"));
    makeAndOpen (pd, "#N canvas 0 0 450 300 12;\n"
                     "#X obj 10 10 loadbang;\n"
                     "#X obj 10 40 metro 100;\n"
                     "#X obj 10 70 s /tick;\n"
                     "#X connect 0 0 1 0;\n"
                     "#X connect 1 0 2 0;\n");

    // The loadbang's first bang is sent as the patch opens.
    const auto atOpen = pd.takeOutbox();
    CHECK (std::count_if (atOpen.sent.begin(), atOpen.sent.end(), [] (const Sent& s) { return s.to == "/tick"; }) == 1);

    int bangs = 0;
    std::vector<int> ticksOfBangs;
    for (int tick = 0; tick < 50; ++tick)
    {
        const auto outbox = tickOnce (pd);
        for (const auto& sent : outbox.sent)
            if (sent.to == "/tick" && sent.selector == "bang")
            {
                ++bangs;
                ticksOfBangs.push_back (tick);
            }
    }
    // A clock runs in the tick its time falls strictly before the end of: the
    // bangs at 100 to 900 ms land in ticks 5 to 45, the one at 1000 ms in the
    // next second's first tick - nine here, and the tenth was the one at open.
    CHECK (bangs == 9);
    REQUIRE (ticksOfBangs.size() >= 2);
    CHECK (ticksOfBangs[0] == 5);
    CHECK (ticksOfBangs[1] - ticksOfBangs[0] == 5);
}

TEST_CASE ("process: Pd reads 2.5 as 2.5 under any locale, and leaves this thread's alone")
{
    const std::string pointBefore = std::localeconv()->decimal_point;

    PdInstance pd (settingsFor ("locale"));
    makeAndOpen (pd, "#N canvas 0 0 450 300 12;\n"
                     "#X obj 10 10 r /a;\n"
                     "#X obj 10 40 * 2.5;\n"
                     "#X obj 10 70 s /b;\n"
                     "#X connect 0 0 1 0;\n"
                     "#X connect 1 0 2 0;\n");
    const auto out = numbersSentTo (tickOnce (pd, { { "/a", { Atom::of (2.0) } } }), "/b");
    REQUIRE (out.size() == 1);
    CHECK (sameNumber (out[0], 5.0));

    CHECK (std::string (std::localeconv()->decimal_point) == pointBefore);
    if (wfgtest::runningUnderFrenchLocale())
        CHECK (pointBefore == ",");
}

TEST_CASE ("process: a patch cannot end Go.dot - pd quit and pd exit are refused, and clocks run on")
{
    PdInstance pd (settingsFor ("quit"));
    makeAndOpen (pd, "#N canvas 0 0 450 300 12;\n"
                     "#X obj 10 10 loadbang;\n"
                     "#X msg 10 40 \\; pd quit \\; pd exit;\n"
                     "#X obj 100 10 loadbang;\n"
                     "#X obj 100 40 metro 20;\n"
                     "#X obj 100 70 s /tick;\n"
                     "#X connect 0 0 1 0;\n"
                     "#X connect 2 0 3 0;\n"
                     "#X connect 3 0 4 0;\n");

    // Still here, and the refusals said in Pd's console.
    const auto atOpen = pd.takeOutbox();
    const auto said = [&] (const char* what)
    {
        return std::any_of (atOpen.printed.begin(), atOpen.printed.end(),
                            [&] (const std::string& line) { return line.find (what) != std::string::npos; });
    };
    CHECK (said ("[pd quit( is refused"));
    CHECK (said ("[pd exit( is refused"));

    // A metro of one tick bangs in every tick after the first.
    int bangs = 0;
    for (int tick = 0; tick < 10; ++tick)
        bangs += static_cast<int> (tickOnce (pd).sent.size());
    CHECK (bangs == 9);
}

TEST_CASE ("process: everything a patch could hear reaches its catch-all as /address atoms")
{
    PdInstance pd (settingsFor ("catchall"));
    makeAndOpen (pd, "#N canvas 0 0 450 300 12;\n"
                     "#X obj 10 10 r in;\n"
                     "#X obj 10 40 route /x;\n"
                     "#X obj 10 70 s /y;\n"
                     "#X connect 0 0 1 0;\n"
                     "#X connect 1 0 2 0;\n");
    const auto outbox = tickOnce (pd, { { "/x", { Atom::of (1.0), Atom::of (2.0) } },
                                        { "/z", { Atom::of (9.0) } } });
    const auto heard = numbersSentTo (outbox, "/y");
    REQUIRE (heard.size() == 2);
    CHECK (sameNumber (heard[0], 1.0));
    CHECK (sameNumber (heard[1], 2.0));
}

TEST_CASE ("process: words, lists and messages leave a patch as Pd sent them")
{
    PdInstance pd (settingsFor ("shapes"));
    makeAndOpen (pd, "#N canvas 0 0 450 300 12;\n"
                     "#X obj 10 10 r /go;\n"
                     "#X msg 10 40 \\; /w symbol left \\; /l 1 2 3 \\; out /dev/x 0.5 up;\n"
                     "#X connect 0 0 1 0;\n");
    const auto outbox = tickOnce (pd, { { "/go", {} } });
    REQUIRE (outbox.sent.size() == 3);
    CHECK (outbox.sent[0].to == "/w");
    CHECK (outbox.sent[0].selector == "symbol");
    CHECK (outbox.sent[0].atoms == Atoms { Atom::of (std::string ("left")) });
    CHECK (outbox.sent[1].to == "/l");
    CHECK (outbox.sent[1].selector == "list");
    CHECK (outbox.sent[1].atoms.size() == 3);
    CHECK (outbox.sent[2].to == "out");
    CHECK (outbox.sent[2].selector == "/dev/x");
    CHECK (outbox.sent[2].atoms == Atoms { Atom::of (0.5), Atom::of (std::string ("up")) });
}

TEST_CASE ("process: a patch's sends past the cap are counted, not kept")
{
    auto settings = settingsFor ("cap");
    settings.maxSent = 5;
    PdInstance pd (settings);
    makeAndOpen (pd, "#N canvas 0 0 450 300 12;\n"
                     "#X obj 10 10 r /go;\n"
                     "#X obj 10 40 until;\n"
                     "#X obj 10 70 s /many;\n"
                     "#X connect 0 0 1 0;\n"
                     "#X connect 1 0 2 0;\n");
    const auto outbox = tickOnce (pd, { { "/go", { Atom::of (12.0) } } });
    CHECK (outbox.sent.size() == 5);
    CHECK (outbox.dropped == 7);
}

TEST_CASE ("process: a job asked for while the last is running is refused, and none before make")
{
    PdInstance pd (settingsFor ("order"));
    CHECK_FALSE (pd.open ("#N canvas 0 0 450 300 12;\n"));
    CHECK_FALSE (pd.tick ({}));
    REQUIRE (pd.make());
    REQUIRE (pd.finished (5s));
    CHECK_FALSE (pd.make());
    CHECK_FALSE (pd.tick ({}));    // nothing open yet
    REQUIRE (pd.open ("#N canvas 0 0 450 300 12;\n"));
    REQUIRE (pd.finished (5s));
    CHECK (pd.tick ({}));
    REQUIRE (pd.finished (5s));
    CHECK (pd.close());
    REQUIRE (pd.finished (5s));
    CHECK_FALSE (pd.made());
}
