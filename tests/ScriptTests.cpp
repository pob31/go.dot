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

//==============================================================================
/*  SC.3: JAVASCRIPT'S INSTANCE ALONE (AMP-AMS). Each driven as the host drives
    it, one job at a time and waited for: one instance is its own quiet point. */
#include <wfg/engine/process/JsLanguage.h>
#include <wfg/engine/process/ScriptInstance.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

namespace
{
    using namespace std::chrono_literals;
    using wfg::process::Atom;
    using wfg::process::Atoms;
    using wfg::process::Input;
    using wfg::process::Outbox;
    using wfg::process::ScriptInstance;
    using wfg::process::Sent;

    std::unique_ptr<ScriptInstance> javascript (ScriptInstance::Settings settings = {})
    {
        return std::make_unique<ScriptInstance> (wfg::process::makeJavascript, settings);
    }

    /*  Make and open, each waited for; what opening sent is handed back. */
    Outbox makeAndOpen (ScriptInstance& js, const std::string& source)
    {
        REQUIRE (js.make());
        REQUIRE (js.finished (5s));
        REQUIRE (js.made());
        REQUIRE (js.open (source));
        REQUIRE (js.finished (5s));
        INFO (js.problem());
        REQUIRE (js.opened());
        return js.takeOutbox();
    }

    /*  Opened again on a new text, waited for. */
    Outbox openAgain (ScriptInstance& js, const std::string& source)
    {
        REQUIRE (js.open (source));
        REQUIRE (js.finished (5s));
        INFO (js.problem());
        REQUIRE (js.opened());
        return js.takeOutbox();
    }

    Outbox tickOnce (ScriptInstance& js, std::vector<Input> inputs = {})
    {
        REQUIRE (js.tick (std::move (inputs)));
        REQUIRE (js.finished (5s));
        return js.takeOutbox();
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

    bool near (double a, double b)
    {
        return std::abs (a - b) < 1e-9;
    }
}

TEST_CASE ("script: a JavaScript script doubles what it hears, on a thread of its own")
{
    auto js = javascript();
    CHECK_FALSE (js->sharesLock());

    const auto atOpen = makeAndOpen (*js, "godot.hear('/a', (x) => godot.send('/b', x * 2));\n");
    CHECK (atOpen.sent.empty());
    CHECK (js->receives() == std::vector<std::string> { "/a" });
    CHECK (js->sends().empty());

    const auto outbox = tickOnce (*js, { { "/a", { Atom::of (21.0) } } });
    REQUIRE (outbox.sent.size() == 1);
    CHECK (outbox.sent[0].to == "/b");
    CHECK (outbox.sent[0].selector == "list");
    CHECK (outbox.sent[0].atoms == Atoms { Atom::of (42.0) });

    //  Nothing heard, nothing sent; and an address it does not hear is passed by.
    CHECK (tickOnce (*js).sent.empty());
    CHECK (tickOnce (*js, { { "/nobody", { Atom::of (1.0) } } }).sent.empty());
}

TEST_CASE ("script: what a script sends - numbers, words, several, none - leaves as the host routes a patch's")
{
    auto js = javascript();
    const auto atOpen = makeAndOpen (*js,
        "godot.send('/dev/x', 0.5, 'up');\n"
        "godot.send('/dev/bang');\n"
        "godot.send('/dev/list', [1, 2], true);\n"
        "godot.set('cue/ABC/level', -6);\n"
        "godot.set('/godot/cue/ABC/name', 'Rain');\n"
        "godot.command('cue.stop', godot.self);\n"
        "godot.fire('3.1');\n"
        "godot.midi.send(0x90, 60, 100);\n");

    REQUIRE (atOpen.sent.size() == 8);
    CHECK (atOpen.sent[0].to == "/dev/x");
    CHECK (atOpen.sent[0].atoms == Atoms { Atom::of (0.5), Atom::of (std::string ("up")) });
    CHECK (atOpen.sent[1].to == "/dev/bang");
    CHECK (atOpen.sent[1].selector == "bang");
    CHECK (atOpen.sent[1].atoms.empty());
    CHECK (atOpen.sent[2].atoms == Atoms { Atom::of (1.0), Atom::of (2.0), Atom::of (1.0) });
    CHECK (atOpen.sent[3].to == "/godot/cue/ABC/level");
    CHECK (atOpen.sent[3].atoms == Atoms { Atom::of (-6.0) });
    CHECK (atOpen.sent[4].to == "/godot/cue/ABC/name");
    CHECK (atOpen.sent[5].to == "/godot/cmd/cue/stop");
    CHECK (atOpen.sent[5].atoms == Atoms { Atom::of (std::string ("self")) });
    CHECK (atOpen.sent[6].to == "/godot/cmd/cue/fire");
    CHECK (atOpen.sent[6].atoms == Atoms { Atom::of (std::string ("3.1")) });
    CHECK (atOpen.sent[7].to == wfg::process::midiName);
    CHECK (atOpen.sent[7].atoms == Atoms { Atom::of (144.0), Atom::of (60.0), Atom::of (100.0) });
}

TEST_CASE ("script: everything heard reaches hearAll as an address and values, and MIDI reaches midi.hear")
{
    auto js = javascript();
    makeAndOpen (*js,
        "godot.hearAll((address, ...values) => godot.send('/all', address, ...values));\n"
        "godot.midi.hear((status, a, b) => godot.send('/midi', status, a, b));\n");
    CHECK (js->receives().empty());

    const auto outbox = tickOnce (*js, { { "/dev/y", { Atom::of (0.25), Atom::of (std::string ("w")) } },
                                         { wfg::process::midiName, { Atom::of (144.0), Atom::of (60.0), Atom::of (1.0) } } });
    REQUIRE (outbox.sent.size() == 2);
    CHECK (outbox.sent[0].to == "/all");
    CHECK (outbox.sent[0].atoms == Atoms { Atom::of (std::string ("/dev/y")), Atom::of (0.25), Atom::of (std::string ("w")) });
    CHECK (outbox.sent[1].to == "/midi");
    CHECK (outbox.sent[1].atoms == Atoms { Atom::of (144.0), Atom::of (60.0), Atom::of (1.0) });
}

TEST_CASE ("script: time is the tick - every 0.1 s is every fifth tick, after 1 s the fiftieth, and a tick counts")
{
    auto js = javascript();
    makeAndOpen (*js,
        "godot.every(0.1, () => godot.send('/every'));\n"
        "const once = godot.after(1, () => godot.send('/after'));\n"
        "const never = godot.after(0.5, () => godot.send('/never'));\n"
        "godot.cancel(never);\n"
        "godot.tick((n) => { if (n === 3) godot.send('/third', n); });\n");

    std::vector<int> every, after, third;
    for (int tick = 1; tick <= 60; ++tick)
    {
        const auto outbox = tickOnce (*js);
        for (const auto& sent : outbox.sent)
        {
            if (sent.to == "/every") every.push_back (tick);
            if (sent.to == "/after") after.push_back (tick);
            if (sent.to == "/third") third.push_back (tick);
            CHECK (sent.to != "/never");
        }
    }
    CHECK (every == std::vector<int> { 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60 });
    CHECK (after == std::vector<int> { 50 });
    CHECK (third == std::vector<int> { 3 });
}

TEST_CASE ("script: JavaScript reads 2.5 as 2.5 and prints it so under any locale")
{
    auto js = javascript();
    const auto atOpen = makeAndOpen (*js, "godot.send('/n', 2.5 * 1); print(2.5, 0.1 + 0.2, 'mot', 1e21);\n");
    REQUIRE (numbersSentTo (atOpen, "/n").size() == 1);
    CHECK (near (numbersSentTo (atOpen, "/n")[0], 2.5));
    REQUIRE (atOpen.printed.size() == 1);
    CHECK (atOpen.printed[0] == "2.5 0.30000000000000004 mot 1e+21");

    //  And a number handed in arrives as itself.
    openAgain (*js, "godot.hear('/x', (x) => print(x, typeof x));\n");
    const auto outbox = tickOnce (*js, { { "/x", { Atom::of (0.125) } } });
    REQUIRE (outbox.printed.size() == 1);
    CHECK (outbox.printed[0] == "0.125 number");
}

TEST_CASE ("script: a script that does not read says why and on which line, and is not run")
{
    auto js = javascript();
    REQUIRE (js->make());
    REQUIRE (js->finished (5s));

    REQUIRE (js->open ("godot.hear('/a', (x) => godot.send('/b', x));\n"
                       "let y = ;\n"));
    REQUIRE (js->finished (5s));
    CHECK_FALSE (js->opened());
    INFO (js->problem());
    CHECK (js->problem().find ("(line 2)") != std::string::npos);
    CHECK (js->receives().empty());
    CHECK_FALSE (js->tick ({}));

    //  A failure at the top level is the same: said with its line, not run.
    REQUIRE (js->open ("\n\nnoSuchThing();\n"));
    REQUIRE (js->finished (5s));
    CHECK_FALSE (js->opened());
    CHECK (js->problem().find ("noSuchThing") != std::string::npos);
    CHECK (js->problem().find ("(line 3)") != std::string::npos);

    //  Opened again on a text that reads, it runs from nothing.
    openAgain (*js, "godot.hear('/a', (x) => godot.send('/b', x));\n");
    CHECK (js->problem().empty());
}

TEST_CASE ("script: a failure inside a script's own function is printed with its line, and the rest run on")
{
    auto js = javascript();
    makeAndOpen (*js,
        "godot.hear('/a', (x) => {\n"
        "    x.no.such;\n"
        "});\n"
        "godot.hear('/a', (x) => godot.send('/b', x));\n");

    const auto outbox = tickOnce (*js, { { "/a", { Atom::of (1.0) } } });
    REQUIRE (outbox.printed.size() == 1);
    INFO (outbox.printed[0]);
    CHECK (outbox.printed[0].find ("(line 2)") != std::string::npos);
    CHECK (numbersSentTo (outbox, "/b") == std::vector<double> { 1.0 });
}

TEST_CASE ("script: the sandbox - no file, no module, no network, and godot cannot be written over")
{
    auto js = javascript();
    const auto atOpen = makeAndOpen (*js,
        "print(typeof std, typeof os, typeof require, typeof fetch, typeof XMLHttpRequest, typeof __host);\n"
        "godot = 1; godot.send = null; godot.midi.send = null; print = null; __godot_tick = null;\n"
        "print(typeof godot, typeof godot.send, typeof godot.midi.send, typeof __godot_tick);\n");
    REQUIRE (atOpen.printed.size() == 2);
    CHECK (atOpen.printed[0] == "undefined undefined undefined undefined undefined undefined");
    CHECK (atOpen.printed[1] == "object function function function");

    //  And a module cannot be imported.
    REQUIRE (js->open ("import * as std from 'std';\n"));
    REQUIRE (js->finished (5s));
    CHECK_FALSE (js->opened());
}

TEST_CASE ("script: a script's sends past the cap are counted, not kept")
{
    ScriptInstance::Settings settings;
    settings.maxSent = 5;
    auto js = javascript (settings);
    makeAndOpen (*js, "godot.hear('/go', (n) => { for (let i = 0; i < n; ++i) godot.send('/many', i); });\n");
    const auto outbox = tickOnce (*js, { { "/go", { Atom::of (12.0) } } });
    CHECK (outbox.sent.size() == 5);
    CHECK (outbox.dropped == 7);
}

TEST_CASE ("script: a job asked for while the last is running is refused, and none before make")
{
    auto js = javascript();
    CHECK_FALSE (js->open ("print(1);"));
    CHECK_FALSE (js->tick ({}));
    REQUIRE (js->make());
    REQUIRE (js->finished (5s));
    CHECK_FALSE (js->make());
    CHECK_FALSE (js->tick ({}));    // nothing open yet
    REQUIRE (js->open ("godot.hear('/slow', () => { const end = Date.now() + 300; while (Date.now() < end) {} });"));
    REQUIRE (js->finished (5s));
    REQUIRE (js->tick ({ { "/slow", {} } }));
    CHECK_FALSE (js->tick ({}));
    CHECK (js->busy());
    REQUIRE (js->finished (5s));
    CHECK (js->close());
    REQUIRE (js->finished (5s));
    CHECK_FALSE (js->made());
}

TEST_CASE ("script: a script stuck in a loop is interrupted - its job ends, and it is freed")
{
    auto js = javascript();
    makeAndOpen (*js, "godot.hear('/loop', () => { for (;;) {} });\n"
                      "godot.hear('/loop', () => godot.send('/after'));\n");

    REQUIRE (js->tick ({ { "/loop", {} } }));
    CHECK_FALSE (js->finished (200ms));
    CHECK (js->busy());

    const auto asked = std::chrono::steady_clock::now();
    CHECK (js->interrupt());
    REQUIRE (js->finished (5s));
    const auto took = std::chrono::steady_clock::now() - asked;
    MESSAGE ("an endless loop interrupted in " << std::chrono::duration<double, std::milli> (took).count() << " ms");
    CHECK (took < 1s);

    //  What it said, and nothing of what came after it in that tick.
    const auto outbox = js->takeOutbox();
    CHECK (std::none_of (outbox.sent.begin(), outbox.sent.end(), [] (const Sent& s) { return s.to == "/after"; }));

    //  Closed and freed as any other.
    REQUIRE (js->close());
    REQUIRE (js->finished (5s));
    CHECK_FALSE (js->made());
}

TEST_CASE ("script: an instance let go while stuck is not waited for - its thread is interrupted and frees it")
{
    auto js = javascript();
    makeAndOpen (*js, "godot.hear('/loop', () => { for (;;) {} });\n");
    REQUIRE (js->tick ({ { "/loop", {} } }));
    CHECK_FALSE (js->finished (100ms));
    js->leaveUnfreed();

    const auto start = std::chrono::steady_clock::now();
    js.reset();
    CHECK (std::chrono::steady_clock::now() - start < 1s);
}

//==============================================================================
/*  SC.8: THE HELP CARD AND THE READY-MADE SCRIPTS (AMW). Read from the repo's
    own `js` folder, as Go.dot reads its copy beside the binary. */
#include <wfg/engine/process/ScriptText.h>

#include <fstream>
#include <iterator>

namespace
{
    std::string readyMadeText (const std::string& name)
    {
        std::ifstream in (std::string (WFG_REPO_ROOT) + "/js/" + name, std::ios::binary);
        return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
    }

    /*  Every function a script can call, as §66.3 lists them. */
    const std::vector<std::string> jsApi {
        "godot.hear(", "godot.hearAll(", "godot.send(", "godot.set(", "godot.command(", "godot.fire(",
        "godot.midi.hear(", "godot.midi.send(", "godot.tick(", "godot.after(", "godot.every(", "godot.cancel(",
        "godot.self", "print(", "console.log("
    };
}

TEST_CASE ("script: the help card names every function a script can call, and the prelude has each")
{
    const auto card = readyMadeText ("godot-help.js");
    REQUIRE_FALSE (card.empty());
    const auto prelude = std::string (wfg::process::javascriptPrelude());

    for (const auto& call : jsApi)
    {
        INFO (call);
        CHECK (card.find (call) != std::string::npos);

        //  In the prelude, as a member of `godot` or a name it leaves.
        auto name = call.substr (call.rfind ('.') == std::string::npos ? 0 : call.rfind ('.') + 1);
        if (! name.empty() && name.back() == '(')
            name.pop_back();
        CHECK ((prelude.find (name + " (") != std::string::npos || prelude.find (name + ":") != std::string::npos
                || prelude.find ("'" + name + "'") != std::string::npos));
    }
}

TEST_CASE ("script: the help card opens as a script and nothing in it runs; the ready-made scripts each open")
{
    const auto names = wfg::process::readyMadeFiles (std::string (WFG_REPO_ROOT) + "/js", "javascript");
    REQUIRE (names.size() >= 5u);
    CHECK (names.front() == "godot-help.js");

    for (const auto& name : names)
    {
        INFO (name);
        auto js = javascript();
        const auto atOpen = makeAndOpen (*js, readyMadeText (name));
        CHECK (atOpen.sent.empty());
        CHECK (atOpen.printed.empty());
        CHECK (js->problem().empty());
    }
}

TEST_CASE ("script: the ready-made scripts do what their comment says")
{
    const auto heard = [] (double x) { return std::vector<Input> { { "/wfs/source/1/x", { Atom::of (x) } } }; };

    {
        auto js = javascript();
        makeAndOpen (*js, readyMadeText ("average.js"));
        CHECK (numbersSentTo (tickOnce (*js, heard (1.0)), "/wfs/source/1/xAverage") == std::vector<double> { 1.0 });
        CHECK (numbersSentTo (tickOnce (*js, heard (2.0)), "/wfs/source/1/xAverage") == std::vector<double> { 1.5 });
        CHECK (numbersSentTo (tickOnce (*js, heard (6.0)), "/wfs/source/1/xAverage") == std::vector<double> { 3.0 });
    }
    {
        auto js = javascript();
        makeAndOpen (*js, readyMadeText ("minmax.js"));
        CHECK (numbersSentTo (tickOnce (*js, heard (5.0)), "/wfs/source/1/xRange") == std::vector<double> { 5.0, 5.0 });
        CHECK (numbersSentTo (tickOnce (*js, heard (1.0)), "/wfs/source/1/xRange") == std::vector<double> { 1.0, 5.0 });
        for (int tick = 0; tick < 100; ++tick)
            tickOnce (*js);
        CHECK (numbersSentTo (tickOnce (*js, heard (3.0)), "/wfs/source/1/xRange") == std::vector<double> { 3.0, 3.0 });
    }
    {
        auto js = javascript();
        makeAndOpen (*js, readyMadeText ("smooth.js"));
        CHECK (numbersSentTo (tickOnce (*js, heard (0.0)), "/wfs/source/1/xSmooth") == std::vector<double> { 0.0 });
        CHECK (numbersSentTo (tickOnce (*js, heard (4.0)), "/wfs/source/1/xSmooth") == std::vector<double> { 1.0 });
    }
    {
        auto js = javascript();
        makeAndOpen (*js, readyMadeText ("edge.js"));
        const auto fired = [] (const Outbox& outbox)
        {
            std::vector<std::string> cues;
            for (const auto& sent : outbox.sent)
                if (sent.to == "/godot/cmd/cue/fire")
                    cues.push_back (sent.atoms.front().word);
            return cues;
        };
        CHECK (fired (tickOnce (*js, heard (0.5))).empty());
        CHECK (fired (tickOnce (*js, heard (0.9))) == std::vector<std::string> { "3.1" });
        CHECK (fired (tickOnce (*js, heard (0.82))).empty());      // in the band: nothing twice
        CHECK (fired (tickOnce (*js, heard (0.95))).empty());
        CHECK (fired (tickOnce (*js, heard (0.7))) == std::vector<std::string> { "3.2" });
    }
}

//==============================================================================
/*  SC.9: LUA'S INSTANCE ALONE (AMX), every JavaScript case again in Lua - the
    same API in Lua's words - and what is Lua's own: whole numbers that stay
    whole, an interrupt pcall cannot catch, a memory cap. */
#include <wfg/engine/process/LuaLanguage.h>

namespace
{
    std::unique_ptr<ScriptInstance> lua (ScriptInstance::Settings settings = {})
    {
        return std::make_unique<ScriptInstance> (wfg::process::makeLua, settings);
    }
}

TEST_CASE ("script: a Lua script doubles what it hears, on a thread of its own")
{
    auto script = lua();
    CHECK_FALSE (script->sharesLock());
    const auto atOpen = makeAndOpen (*script, "godot.hear('/a', function (x) godot.send('/b', x * 2) end)\n");
    CHECK (atOpen.sent.empty());
    CHECK (script->receives() == std::vector<std::string> { "/a" });

    const auto outbox = tickOnce (*script, { { "/a", { Atom::of (21.0) } } });
    REQUIRE (outbox.sent.size() == 1);
    CHECK (outbox.sent[0].to == "/b");
    CHECK (outbox.sent[0].atoms == Atoms { Atom::of (42.0) });
    CHECK (tickOnce (*script).sent.empty());
    CHECK (tickOnce (*script, { { "/nobody", { Atom::of (1.0) } } }).sent.empty());
}

TEST_CASE ("script: what a Lua script sends leaves as JavaScript's does")
{
    auto script = lua();
    const auto atOpen = makeAndOpen (*script,
        "godot.send('/dev/x', 0.5, 'up')\n"
        "godot.send('/dev/bang')\n"
        "godot.send('/dev/list', {1, 2}, true)\n"
        "godot.set('cue/ABC/level', -6)\n"
        "godot.set('/godot/cue/ABC/name', 'Rain')\n"
        "godot.command('cue.stop', godot.self)\n"
        "godot.fire('3.1')\n"
        "godot.midi.send(0x90, 60, 100)\n");

    REQUIRE (atOpen.sent.size() == 8);
    CHECK (atOpen.sent[0].atoms == Atoms { Atom::of (0.5), Atom::of (std::string ("up")) });
    CHECK (atOpen.sent[1].selector == "bang");
    CHECK (atOpen.sent[2].atoms == Atoms { Atom::of (1.0), Atom::of (2.0), Atom::of (1.0) });
    CHECK (atOpen.sent[3].to == "/godot/cue/ABC/level");
    CHECK (atOpen.sent[4].to == "/godot/cue/ABC/name");
    CHECK (atOpen.sent[5].to == "/godot/cmd/cue/stop");
    CHECK (atOpen.sent[5].atoms == Atoms { Atom::of (std::string ("self")) });
    CHECK (atOpen.sent[6].to == "/godot/cmd/cue/fire");
    CHECK (atOpen.sent[7].to == wfg::process::midiName);
    CHECK (atOpen.sent[7].atoms == Atoms { Atom::of (144.0), Atom::of (60.0), Atom::of (100.0) });
}

TEST_CASE ("script: in Lua everything heard reaches hearAll, MIDI reaches midi.hear, and whole numbers stay whole")
{
    auto script = lua();
    makeAndOpen (*script,
        "godot.hearAll(function (address, ...) godot.send('/all', address, ...) end)\n"
        "godot.midi.hear(function (status, a, b) print(status, a, b, math.type(a)) end)\n");

    const auto outbox = tickOnce (*script, { { "/dev/y", { Atom::of (0.25), Atom::of (std::string ("w")) } },
                                             { wfg::process::midiName, { Atom::of (144.0), Atom::of (60.0), Atom::of (1.0) } } });
    REQUIRE (outbox.sent.size() == 1);
    CHECK (outbox.sent[0].atoms == Atoms { Atom::of (std::string ("/dev/y")), Atom::of (0.25), Atom::of (std::string ("w")) });
    REQUIRE (outbox.printed.size() == 1);
    CHECK (outbox.printed[0] == "144 60 1 integer");
}

TEST_CASE ("script: in Lua time is the tick too")
{
    auto script = lua();
    makeAndOpen (*script,
        "godot.every(0.1, function () godot.send('/every') end)\n"
        "godot.after(1, function () godot.send('/after') end)\n"
        "local never = godot.after(0.5, function () godot.send('/never') end)\n"
        "godot.cancel(never)\n"
        "godot.tick(function (n) if n == 3 then godot.send('/third', n) end end)\n");

    std::vector<int> every, after, third;
    for (int tick = 1; tick <= 60; ++tick)
        for (const auto& sent : tickOnce (*script).sent)
        {
            if (sent.to == "/every") every.push_back (tick);
            if (sent.to == "/after") after.push_back (tick);
            if (sent.to == "/third") third.push_back (tick);
            CHECK (sent.to != "/never");
        }
    CHECK (every == std::vector<int> { 5, 10, 15, 20, 25, 30, 35, 40, 45, 50, 55, 60 });
    CHECK (after == std::vector<int> { 50 });
    CHECK (third == std::vector<int> { 3 });
}

TEST_CASE ("script: Lua reads 2.5 as 2.5 and prints it so under any locale")
{
    auto script = lua();
    const auto atOpen = makeAndOpen (*script, "godot.send('/n', 2.5 * 1) print(2.5, 0.1 + 0.2, 'mot', 1e21, 7 // 2)\n");
    REQUIRE (numbersSentTo (atOpen, "/n").size() == 1);
    CHECK (near (numbersSentTo (atOpen, "/n")[0], 2.5));
    REQUIRE (atOpen.printed.size() == 1);
    CHECK (atOpen.printed[0] == "2.5 0.3 mot 1e+21 3");

    openAgain (*script, "godot.hear('/x', function (x) print(x, math.type(x)) end)\n");
    const auto outbox = tickOnce (*script, { { "/x", { Atom::of (0.125) } } });
    REQUIRE (outbox.printed.size() == 1);
    CHECK (outbox.printed[0] == "0.125 float");
}

TEST_CASE ("script: a Lua script that does not read says why and on which line, and is not run")
{
    auto script = lua();
    REQUIRE (script->make());
    REQUIRE (script->finished (5s));

    REQUIRE (script->open ("godot.hear('/a', function (x) godot.send('/b', x) end)\nlocal y = \n"));
    REQUIRE (script->finished (5s));
    CHECK_FALSE (script->opened());
    INFO (script->problem());
    CHECK (script->problem().find ("(line 3)") != std::string::npos);
    CHECK (script->receives().empty());

    REQUIRE (script->open ("\n\nnoSuchThing()\n"));
    REQUIRE (script->finished (5s));
    CHECK_FALSE (script->opened());
    CHECK (script->problem().find ("noSuchThing") != std::string::npos);
    CHECK (script->problem().find ("(line 3)") != std::string::npos);

    openAgain (*script, "godot.hear('/a', function (x) godot.send('/b', x) end)\n");
    CHECK (script->problem().empty());
}

TEST_CASE ("script: a failure inside a Lua script's own function is printed with its line, and the rest run on")
{
    auto script = lua();
    makeAndOpen (*script,
        "godot.hear('/a', function (x)\n"
        "    local t = x.no.such\n"
        "end)\n"
        "godot.hear('/a', function (x) godot.send('/b', x) end)\n");

    const auto outbox = tickOnce (*script, { { "/a", { Atom::of (1.0) } } });
    REQUIRE (outbox.printed.size() == 1);
    INFO (outbox.printed[0]);
    CHECK (outbox.printed[0].find ("(line 2)") != std::string::npos);
    CHECK (numbersSentTo (outbox, "/b") == std::vector<double> { 1.0 });
}

TEST_CASE ("script: the Lua sandbox - no io, os, package, debug, file or binary chunk, and godot cannot be changed")
{
    auto script = lua();
    const auto atOpen = makeAndOpen (*script,
        "print(type(io), type(os), type(package), type(debug), type(require), type(dofile), type(loadfile), type(string.dump))\n"
        "print(pcall(function () godot.send = nil end))\n"
        "print(load('\\27Lua'))\n"
        "print(type(godot.send), type(godot.midi.send), math.type(1), utf8.char(233), coroutine.isyieldable())\n");
    REQUIRE (atOpen.printed.size() == 4);
    CHECK (atOpen.printed[0] == "nil nil nil nil nil nil nil nil");
    CHECK (atOpen.printed[1].rfind ("false", 0) == 0);
    CHECK (atOpen.printed[1].find ("godot cannot be changed") != std::string::npos);
    CHECK (atOpen.printed[2].rfind ("nil", 0) == 0);
    CHECK (atOpen.printed[3].rfind ("function function integer", 0) == 0);
}

TEST_CASE ("script: a Lua script's sends past the cap are counted, not kept")
{
    ScriptInstance::Settings settings;
    settings.maxSent = 5;
    auto script = lua (settings);
    makeAndOpen (*script, "godot.hear('/go', function (n) for i = 1, n do godot.send('/many', i) end end)\n");
    const auto outbox = tickOnce (*script, { { "/go", { Atom::of (12.0) } } });
    CHECK (outbox.sent.size() == 5);
    CHECK (outbox.dropped == 7);
}

TEST_CASE ("script: a Lua script stuck in a loop is interrupted - pcall and coroutines cannot keep it - and freed")
{
    for (const auto* loop : { "godot.hear('/loop', function () while true do end end)\n",
                              "godot.hear('/loop', function () while true do pcall(function () while true do end end) end end)\n",
                              "godot.hear('/loop', function () while true do coroutine.resume(coroutine.create(function () while true do end end)) end end)\n" })
    {
        INFO (loop);
        auto script = lua();
        makeAndOpen (*script, std::string (loop) + "godot.hear('/loop', function () godot.send('/after') end)\n");
        REQUIRE (script->tick ({ { "/loop", {} } }));
        CHECK_FALSE (script->finished (200ms));

        const auto asked = std::chrono::steady_clock::now();
        CHECK (script->interrupt());
        REQUIRE (script->finished (5s));
        CHECK (std::chrono::steady_clock::now() - asked < 1s);

        const auto outbox = script->takeOutbox();
        CHECK (std::none_of (outbox.sent.begin(), outbox.sent.end(), [] (const Sent& s) { return s.to == "/after"; }));
        REQUIRE (script->close());
        REQUIRE (script->finished (5s));
        CHECK_FALSE (script->made());
    }
}

TEST_CASE ("script: a Lua script that eats memory is stopped by the cap, and says so")
{
    auto script = lua();
    makeAndOpen (*script, "godot.hear('/eat', function () local t = {} for i = 1, 1e9 do t[i] = string.rep('x', 1000) .. i end end)\n"
                          "godot.hear('/fine', function () godot.send('/ok') end)\n");
    const auto outbox = tickOnce (*script, { { "/eat", {} } });
    REQUIRE_FALSE (outbox.printed.empty());
    CHECK (outbox.printed[0].find ("memory") != std::string::npos);

    //  And it runs on afterwards.
    CHECK (tickOnce (*script, { { "/fine", {} } }).sent.size() == 1u);
}

TEST_CASE ("script: Lua's prelude has every function the JavaScript card names")
{
    const auto prelude = std::string (wfg::process::luaPrelude());
    for (const auto* name : { "hear", "hearAll", "send", "set", "command", "fire", "tick", "after", "every", "cancel", "self" })
    {
        INFO (name);
        CHECK ((prelude.find (std::string (name) + " = function") != std::string::npos
                || prelude.find (std::string (name) + " = '") != std::string::npos));
    }
    CHECK (wfg::process::luaProblem ("script:7: attempt to index a nil value") == "attempt to index a nil value (line 7)");
    CHECK (wfg::process::luaProblem ("not a position") == "not a position");
}

//==============================================================================
/*  SC.10: LUA'S HELP CARD AND READY-MADE SCRIPTS (AMW), as JavaScript's. */
namespace
{
    std::string luaReadyMade (const std::string& name)
    {
        std::ifstream in (std::string (WFG_REPO_ROOT) + "/lua/" + name, std::ios::binary);
        return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
    }
}

TEST_CASE ("script: the Lua help card names every function a script can call, and nothing in it runs")
{
    const auto card = luaReadyMade ("godot-help.lua");
    REQUIRE_FALSE (card.empty());
    for (const auto& call : jsApi)
    {
        if (call == "console.log(")
            continue;   // JavaScript's alone
        INFO (call);
        CHECK (card.find (call) != std::string::npos);
    }

    const auto names = wfg::process::readyMadeFiles (std::string (WFG_REPO_ROOT) + "/lua", "lua");
    REQUIRE (names.size() >= 5u);
    CHECK (names.front() == "godot-help.lua");
    for (const auto& name : names)
    {
        INFO (name);
        auto script = lua();
        const auto atOpen = makeAndOpen (*script, luaReadyMade (name));
        CHECK (atOpen.sent.empty());
        CHECK (atOpen.printed.empty());
    }
}

TEST_CASE ("script: the Lua ready-made scripts do what JavaScript's do")
{
    const auto heard = [] (double x) { return std::vector<Input> { { "/wfs/source/1/x", { Atom::of (x) } } }; };
    {
        auto script = lua();
        makeAndOpen (*script, luaReadyMade ("average.lua"));
        CHECK (numbersSentTo (tickOnce (*script, heard (1.0)), "/wfs/source/1/xAverage") == std::vector<double> { 1.0 });
        CHECK (numbersSentTo (tickOnce (*script, heard (2.0)), "/wfs/source/1/xAverage") == std::vector<double> { 1.5 });
        CHECK (numbersSentTo (tickOnce (*script, heard (6.0)), "/wfs/source/1/xAverage") == std::vector<double> { 3.0 });
    }
    {
        auto script = lua();
        makeAndOpen (*script, luaReadyMade ("minmax.lua"));
        CHECK (numbersSentTo (tickOnce (*script, heard (5.0)), "/wfs/source/1/xRange") == std::vector<double> { 5.0, 5.0 });
        CHECK (numbersSentTo (tickOnce (*script, heard (1.0)), "/wfs/source/1/xRange") == std::vector<double> { 1.0, 5.0 });
        for (int tick = 0; tick < 100; ++tick)
            tickOnce (*script);
        CHECK (numbersSentTo (tickOnce (*script, heard (3.0)), "/wfs/source/1/xRange") == std::vector<double> { 3.0, 3.0 });
    }
    {
        auto script = lua();
        makeAndOpen (*script, luaReadyMade ("smooth.lua"));
        CHECK (numbersSentTo (tickOnce (*script, heard (0.0)), "/wfs/source/1/xSmooth") == std::vector<double> { 0.0 });
        CHECK (numbersSentTo (tickOnce (*script, heard (4.0)), "/wfs/source/1/xSmooth") == std::vector<double> { 1.0 });
    }
    {
        auto script = lua();
        makeAndOpen (*script, luaReadyMade ("edge.lua"));
        const auto fired = [] (const Outbox& outbox)
        {
            std::vector<std::string> cues;
            for (const auto& sent : outbox.sent)
                if (sent.to == "/godot/cmd/cue/fire")
                    cues.push_back (sent.atoms.front().word);
            return cues;
        };
        CHECK (fired (tickOnce (*script, heard (0.5))).empty());
        CHECK (fired (tickOnce (*script, heard (0.9))) == std::vector<std::string> { "3.1" });
        CHECK (fired (tickOnce (*script, heard (0.82))).empty());
        CHECK (fired (tickOnce (*script, heard (0.7))) == std::vector<std::string> { "3.2" });
    }
}

TEST_CASE ("script: a Lua script's starter opens and does nothing, and its copied lines are Lua")
{
    auto script = lua();
    const auto atOpen = makeAndOpen (*script, wfg::process::starterScript ("lua"));
    CHECK (atOpen.sent.empty());
    openAgain (*script, wfg::process::scriptLineToFireCue ("lua", "B3NK0001") + "\n"
                        + wfg::process::scriptLinesToHearSerial ("lua", "SRL00001") + "\n"
                        + wfg::process::scriptLineToSendSerial ("lua", "SRL00001") + "\n");
    CHECK (script->receives() == std::vector<std::string> { "/godot/serial/SRL00001/in" });

    //  And JavaScript's, in JavaScript.
    auto js = javascript();
    makeAndOpen (*js, wfg::process::starterScript ("javascript"));
    openAgain (*js, wfg::process::scriptLineToFireCue ("javascript", "B3NK0001") + "\n"
                    + wfg::process::scriptLinesToHearSerial ("javascript", "SRL00001") + "\n"
                    + wfg::process::scriptLineToSendSerial ("javascript", "SRL00001") + "\n");
    CHECK (js->receives() == std::vector<std::string> { "/godot/serial/SRL00001/in" });
}
