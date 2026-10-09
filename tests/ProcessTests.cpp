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

#include <wfg/engine/Engine.h>
#include <wfg/engine/cue/CueCommands.h>
#include <wfg/engine/cue/CueList.h>
#include <wfg/engine/cue/Run.h>
#include <wfg/engine/cue/RunCommands.h>
#include <wfg/engine/cue/Runner.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/midi/MidiSink.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/process/PatchText.h>
#include <wfg/engine/process/PdInstance.h>
#include <wfg/engine/process/ProcessHost.h>
#include <wfg/engine/tree/Mount.h>
#include <wfg/engine/tree/MountSender.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
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

    /*  ONE FOLDER PER LOCALE RUN: ctest runs this binary under C and fr-FR at
        once, and two processes writing one patch file would open each other's. */
    std::filesystem::path scratch (const std::string& name)
    {
        std::string run = wfgtest::appliedLocaleName();
        std::replace_if (run.begin(), run.end(), [] (char c) { return ! std::isalnum (static_cast<unsigned char> (c)); }, '-');
        return std::filesystem::temp_directory_path() / ("wfg-process-tests-" + run) / name;
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

TEST_CASE ("process: a number Pd works out leaves as the shortest decimal it is, not a float's tail")
{
    PdInstance pd (settingsFor ("shortest"));
    makeAndOpen (pd, "#N canvas 0 0 450 300 12;\n"
                     "#X obj 10 10 r /a;\n"
                     "#X obj 10 40 * 2;\n"
                     "#X obj 10 70 s /b;\n"
                     "#X connect 0 0 1 0;\n"
                     "#X connect 1 0 2 0;\n");
    const auto out = numbersSentTo (tickOnce (pd, { { "/a", { Atom::of (0.05) } } }), "/b");
    REQUIRE (out.size() == 1);

    // Pd holds 0.1 as the float 0.100000001490116; Go.dot is handed 0.1.
    const bool exactlyATenth = ! (out[0] < 0.1) && ! (0.1 < out[0]);
    CHECK (exactlyATenth);
}

//==============================================================================
/*  PC.2: THE PROCESS CUE. A show with one device and the cues a patch plays
    with; everything below the command is real - a mount table with a parsed
    namespace, a sender on a loopback socket, the Runner's hook and a host
    running Pd instances on threads of their own. */
namespace
{
    constexpr const char* deviceJson = R"JSON({
      "FULL_PATH": "/",
      "CONTENTS": {
        "in":  { "FULL_PATH": "/in",  "TYPE": "f", "ACCESS": 3 },
        "out": { "FULL_PATH": "/out", "TYPE": "f", "ACCESS": 3 }
      }
    })JSON";

    constexpr const char* deviceId = "K3PV7WRB";

    wfg::tree::MountDeclaration deviceMount (int port)
    {
        wfg::tree::MountDeclaration mount;
        mount.id = deviceId;
        mount.prefix = "/dev";
        mount.namespaceFile = "namespaces/dev.json";
        mount.host = "127.0.0.1";
        mount.port = port;
        return mount;
    }

    /*  A socket that keeps what it was sent. */
    struct Listener
    {
        Listener()
        {
            const auto started = endpoint.start (0, [this] (wfg::osc::Datagram datagram)
                                                    {
                                                        const std::lock_guard<std::mutex> lock { guard };
                                                        received.push_back (std::move (datagram));
                                                    });
            REQUIRE (started);
        }

        ~Listener() { endpoint.stop(); }

        int port() const { return endpoint.boundPort(); }

        std::vector<wfg::osc::Packet> packets() const
        {
            const std::lock_guard<std::mutex> lock { guard };
            std::vector<wfg::osc::Packet> out;
            for (const auto& datagram : received)
            {
                const auto decoded = wfg::osc::decode (datagram.bytes.data(), datagram.bytes.size());
                if (decoded.ok)
                    out.push_back (decoded.packet);
            }
            return out;
        }

        /*  The first value sent to `address`, waited for. */
        std::optional<double> valueAt (const std::string& address, int millisecondsAtMost = 3000) const
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (millisecondsAtMost);
            while (std::chrono::steady_clock::now() < deadline)
            {
                for (const auto& packet : packets())
                    if (packet.address == address && ! packet.args.empty())
                        return packet.args.front().asDouble();
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }
            return std::nullopt;
        }

        void clear()
        {
            const std::lock_guard<std::mutex> lock { guard };
            received.clear();
        }

        mutable std::mutex guard;
        std::vector<wfg::osc::Datagram> received;
        wfg::osc::UdpEndpoint endpoint;
    };

    /*  What a patch sent on its MIDI port, kept. */
    struct RecordingSink : wfg::midi::MidiSink
    {
        std::string send (const std::string& port, const wfg::midi::Bytes& bytes) override
        {
            sent.emplace_back (port, bytes);
            return {};
        }

        std::vector<std::pair<std::string, wfg::midi::Bytes>> sent;
    };

    wfg::process::ProcessHost::Settings hostSettings (const std::string& name)
    {
        wfg::process::ProcessHost::Settings settings;
        settings.cacheFolder = scratch (name).string();
        return settings;
    }

    struct ProcessRig
    {
        explicit ProcessRig (const std::string& name, bool withHost = true)
            : sender (listener.endpoint),
              host (hostSettings (name))
        {
            REQUIRE (mounts.load (deviceMount (listener.port()), deviceJson).ok);

            engine.log().openInMemory ({});
            wfg::doc::registerDocumentCommands (engine.commands(), document, foreignWrite());
            wfg::cue::registerCueCommands (engine.commands(), document, focus);
            wfg::cue::registerRunCommands (engine.commands(), runs);
            wfg::cue::registerGoCommands (engine.commands(), engine, runner, document, focus, runIds);

            runner.setMounts (&mounts, &sender);
            runner.setMidiSink (&midiSink);
            runner.setProcessMidi (&midiInbox);
            if (withHost)
                runner.setProcesses (&host);

            listId = document.createList ("Sound").id;

            //  A Debug build makes a Pd instance slower than a show's budget.
            document.setAttribute ("/godot/list/processBudget", "20");
            document.setAttribute ("/godot/list/goDebounce", "0");
        }

        wfg::doc::ForeignWrite foreignWrite()
        {
            return [this] (const std::string& address, const wfg::osc::Values& values)
            {
                const auto written = wfg::tree::writeToDevice (mounts, sender, address, values);
                if (! written.ok)
                    return wfg::Outcome::rejected (written.reason);
                std::vector<wfg::osc::Value> applied { wfg::osc::Value::string (address) };
                applied.insert (applied.end(), written.values.begin(), written.values.end());
                return wfg::Outcome::ok (std::move (applied));
            };
        }

        std::string makeProcess (const std::string& patch, const std::string& parent = {})
        {
            const auto id = document.createCue (parent.empty() ? listId : parent,
                                                parent.empty() ? index++ : 0, "process", "Patch").id;
            REQUIRE (! id.empty());
            document.setAttribute ("/godot/cue/" + id + "/patch", patch);
            return id;
        }

        std::string makeMemo (const std::string& name)
        {
            const auto id = document.createCue (listId, index++, "memo", name).id;
            REQUIRE (! id.empty());
            return id;
        }

        void tickOnce()
        {
            runner.beforeTick (engine, tick);
            engine.processTick (tick++);
            sender.flush();
        }

        void submit (const std::string& name, std::vector<wfg::osc::Value> args = {})
        {
            REQUIRE (engine.submit ("cli", name, std::move (args)));
            tickOnce();
        }

        void fire (const std::string& cueId)
        {
            submit ("cue.fire", { wfg::osc::Value::string (cueId) });
        }

        /*  What a device with rx on reports, as `mount.heard`'s handler keeps
            it: on the tick just run, so the next tick's hook hands it on. */
        void hear (const std::string& address, float value)
        {
            mounts.noteObservation (address, { wfg::osc::Value::float32 (value) }, tick - 1, -1);
            mounts.noteHeard (deviceId, address, tick - 1);
        }

        const wfg::cue::Run* runOf (const std::string& cueId) const
        {
            const wfg::cue::Run* last = nullptr;
            for (const auto& run : runs.all())
                if (run.cue == cueId)
                    last = &run;
            return last;
        }

        std::size_t runsOf (const std::string& cueId) const
        {
            return static_cast<std::size_t> (std::count_if (runs.all().begin(), runs.all().end(),
                                                            [&] (const auto& run) { return run.cue == cueId; }));
        }

        bool tickUntil (const std::function<bool()>& done, int ticks = 300)
        {
            for (int n = 0; n < ticks; ++n)
            {
                if (done())
                    return true;
                tickOnce();
            }
            return done();
        }

        bool running (const std::string& cueId)
        {
            return tickUntil ([&] { const auto* run = runOf (cueId); return run != nullptr && run->processState == "running"; });
        }

        std::vector<wfg::LogRecord> records()
        {
            return wfg::LogFile::parse (engine.log().contents()).records;
        }

        Listener listener;
        wfg::tree::MountTable mounts;
        wfg::tree::MountSender sender;
        wfg::process::ProcessHost host;
        wfg::process::MidiInbox midiInbox;
        RecordingSink midiSink;

        wfg::Engine engine;
        wfg::doc::ShowDocument document;
        wfg::cue::RunTable runs;
        wfg::cue::Focus focus;
        wfg::doc::IdRegistry runIds = wfg::doc::IdRegistry::withSeed (17);
        wfg::cue::Runner runner { document, runs, runIds, focus };

        std::string listId;
        int index = 0;
        std::int64_t tick = 1;
    };

    std::string doubling (const char* times = "2")
    {
        return std::string ("#N canvas 0 0 450 300 12;\n"
                            "#X obj 10 10 r /dev/in;\n"
                            "#X obj 10 40 * ") + times + ";\n"
               "#X obj 10 70 s /dev/out;\n"
               "#X connect 0 0 1 0;\n"
               "#X connect 1 0 2 0;\n";
    }
}

TEST_CASE ("process cue: a patch doubles what a device reports, on the wire the tick it is heard")
{
    ProcessRig rig ("pc2-double");
    const auto cue = rig.makeProcess (doubling());

    rig.fire (cue);
    REQUIRE (rig.runOf (cue) != nullptr);
    CHECK (rig.runOf (cue)->kind == "process");
    CHECK (rig.runOf (cue)->state == wfg::cue::runState::playing);
    REQUIRE (rig.running (cue));

    rig.listener.clear();
    rig.hear ("/dev/in", 0.25f);
    rig.tickOnce();

    const auto sent = rig.listener.valueAt ("/dev/out");
    REQUIRE (sent.has_value());
    CHECK (sameNumber (*sent, 0.5));

    // In the tree as a cue's write would be.
    const auto* value = rig.mounts.valueOf ("/dev/out");
    REQUIRE (value != nullptr);
    CHECK (sameNumber (value->front().asDouble(), 0.5));

}

TEST_CASE ("process cue: a patch fires a cue by name, under its own origin, and self is its own")
{
    ProcessRig rig ("pc2-fire");
    const auto bell = rig.makeMemo ("Bell");
    const auto cue = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                      "#X obj 10 10 r /dev/in;\n"
                                      "#X msg 10 40 \\; /godot/cmd/cue/fire " + bell + ";\n"
                                      "#X connect 0 0 1 0;\n");
    rig.fire (cue);
    REQUIRE (rig.running (cue));
    const auto processRun = rig.runOf (cue)->id;

    CHECK (rig.runsOf (bell) == 0u);
    rig.hear ("/dev/in", 1.0f);
    rig.tickOnce();
    rig.tickOnce();
    CHECK (rig.runsOf (bell) == 1u);

    const auto records = rig.records();
    const auto fired = std::find_if (records.begin(), records.end(), [&] (const auto& r)
    {
        return r.command == "cue.fire" && ! r.args.empty() && r.args.front().getString() == bell;
    });
    REQUIRE (fired != records.end());
    CHECK (fired->origin == "process:" + processRun);
}

TEST_CASE ("process cue: a patch ends its own run with self, and that is its completion")
{
    ProcessRig rig ("pc2-self");
    const auto cue = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                      "#X obj 10 10 r /dev/in;\n"
                                      "#X msg 10 40 \\; /godot/cmd/run/stop self;\n"
                                      "#X connect 0 0 1 0;\n");
    rig.fire (cue);
    REQUIRE (rig.running (cue));
    rig.hear ("/dev/in", 1.0f);
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (cue)->isFinished(); }, 10));
    CHECK (rig.runOf (cue)->state == wfg::cue::runState::done);
}

TEST_CASE ("process cue: a disable cue a patch fires switches its target off for this run")
{
    ProcessRig rig ("pc2-disable");
    const auto bell = rig.makeMemo ("Bell");
    const auto off = rig.document.createCue (rig.listId, rig.index++, "transport", "Off").id;
    rig.document.setAttribute ("/godot/cue/" + off + "/verb", "disable");
    rig.document.setAttribute ("/godot/cue/" + off + "/target", bell);

    const auto cue = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                      "#X obj 10 10 r /dev/in;\n"
                                      "#X msg 10 40 \\; /godot/cmd/cue/fire " + off + ";\n"
                                      "#X connect 0 0 1 0;\n");
    rig.fire (cue);
    REQUIRE (rig.running (cue));
    rig.hear ("/dev/in", 1.0f);
    rig.tickOnce();
    rig.tickOnce();

    // The bell is off: a fire by name is refused, and said so.
    REQUIRE (rig.engine.submit ("cli", "cue.fire", { wfg::osc::Value::string (bell) }));
    rig.tickOnce();
    const auto records = rig.records();
    CHECK (std::any_of (records.begin(), records.end(), [&] (const auto& r)
    {
        return r.kind == wfg::LogRecord::Kind::rejected && r.command == "cue.fire" && r.reason == "disabled";
    }));
    CHECK (rig.runsOf (bell) == 0u);
}

TEST_CASE ("process cue: Esc ends the run at once and its patch is closed; a second GO does nothing")
{
    ProcessRig rig ("pc2-esc");
    const auto cue = rig.makeProcess (doubling());
    rig.fire (cue);
    REQUIRE (rig.running (cue));

    rig.fire (cue);
    CHECK (rig.runsOf (cue) == 1u);
    CHECK (rig.host.instances() == 1u);

    rig.submit ("run.stopAll");
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (cue)->isFinished(); }, 5));
    REQUIRE (rig.tickUntil ([&] { return rig.host.instances() == 0u; }, 20));

    // Nothing it would have sent leaves now.
    rig.listener.clear();
    rig.hear ("/dev/in", 0.25f);
    rig.tickOnce();
    CHECK_FALSE (rig.listener.valueAt ("/dev/out", 100).has_value());
}

TEST_CASE ("process cue: an edit opens the running patch again")
{
    ProcessRig rig ("pc2-edit");
    const auto cue = rig.makeProcess (doubling());
    rig.fire (cue);
    REQUIRE (rig.running (cue));

    rig.document.setAttribute ("/godot/cue/" + cue + "/patch", doubling ("3"));

    std::optional<double> seen;
    for (int n = 0; n < 50 && ! (seen.has_value() && sameNumber (*seen, 0.75)); ++n)
    {
        rig.listener.clear();
        rig.hear ("/dev/in", 0.25f);
        rig.tickOnce();
        seen = rig.listener.valueAt ("/dev/out", 200);
    }
    REQUIRE (seen.has_value());
    CHECK (sameNumber (*seen, 0.75));
    CHECK (rig.runsOf (cue) == 1u);
}

TEST_CASE ("process cue: in the persistent section it is started again at the next step")
{
    ProcessRig rig ("pc2-persistent");
    const auto memo = rig.makeMemo ("Scene");
    const auto section = rig.document.createPersistent (rig.listId);
    REQUIRE (section.ok);
    const auto cue = rig.makeProcess (doubling(), section.id);

    const auto step = [&]
    {
        rig.submit ("standby.set", { wfg::osc::Value::string (memo) });
        rig.submit ("go");
        rig.tickOnce();
    };

    step();
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (cue) != nullptr; }, 40));
    const auto first = rig.runOf (cue)->id;
    REQUIRE (rig.running (cue));

    // Esc stops it; the next step puts it back - a new run, from its beginning.
    rig.submit ("run.stopAll");
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (cue)->isFinished(); }, 5));
    step();
    REQUIRE (rig.tickUntil ([&] { const auto* run = rig.runOf (cue); return run != nullptr && run->id != first; }, 40));
    REQUIRE (rig.running (cue));

    rig.listener.clear();
    rig.hear ("/dev/in", 0.25f);
    rig.tickOnce();
    const auto sent = rig.listener.valueAt ("/dev/out");
    REQUIRE (sent.has_value());
    CHECK (sameNumber (*sent, 0.5));
}

TEST_CASE ("process cue: with no host - a replay - the run plays and ends as the log says")
{
    ProcessRig rig ("pc2-replay", false);
    const auto cue = rig.makeProcess (doubling());
    rig.fire (cue);
    REQUIRE (rig.runOf (cue) != nullptr);
    CHECK (rig.runOf (cue)->state == wfg::cue::runState::playing);
    CHECK (rig.runOf (cue)->processState == "starting");

    rig.hear ("/dev/in", 0.25f);
    rig.tickOnce();
    CHECK_FALSE (rig.listener.valueAt ("/dev/out", 100).has_value());

    rig.submit ("run.stopAll");
    REQUIRE (rig.tickUntil ([&] { return rig.runOf (cue)->isFinished(); }, 5));
}

TEST_CASE ("process cue: a stuck patch fails its run and holds Pd, and the show never waits on it")
{
    using clock = std::chrono::steady_clock;
    {
        ProcessRig rig ("pc2-stuck");
        rig.document.setAttribute ("/godot/list/processStuckAfter", "5");

        const auto steady = rig.makeProcess (doubling());
        rig.fire (steady);
        REQUIRE (rig.running (steady));

        // Fifty million passes of [until]: seconds of Pd, far past five ticks and
        // their hundred milliseconds.
        const auto loop = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                           "#X obj 10 10 r /dev/in;\n"
                                           "#X msg 10 40 50000000;\n"
                                           "#X obj 10 70 until;\n"
                                           "#X connect 0 0 1 0;\n"
                                           "#X connect 1 0 2 0;\n");
        rig.fire (loop);
        REQUIRE (rig.running (loop));
        rig.document.setAttribute ("/godot/list/processBudget", "2");

        rig.hear ("/dev/in", 1.0f);
        auto longest = clock::duration::zero();
        // Ticks a couple of milliseconds apart, as a show's are twenty: stuck is
        // late for five ticks and as long in time.
        for (int n = 0; n < 500 && ! rig.runOf (loop)->isFinished(); ++n)
        {
            const auto started = clock::now();
            rig.tickOnce();
            longest = std::max (longest, clock::now() - started);
            std::this_thread::sleep_for (2ms);
        }

        const auto* stuck = rig.runOf (loop);
        CHECK (stuck->state == wfg::cue::runState::failed);
        CHECK (stuck->error == "process-stuck");

        // The tick waited for the patches at most the budget, never the loop.
        CHECK (longest < std::chrono::milliseconds (200));

        // A patch already running carries on.
        rig.listener.clear();
        rig.hear ("/dev/in", 0.25f);
        rig.tickOnce();
        const auto sent = rig.listener.valueAt ("/dev/out");
        REQUIRE (sent.has_value());
        CHECK (sameNumber (*sent, 0.5));

        // And nothing new is opened: it would wait for ever, and stop every patch.
        const auto late = rig.makeProcess (doubling());
        rig.fire (late);
        REQUIRE (rig.tickUntil ([&] { return rig.runOf (late)->isFinished(); }, 10));
        CHECK (rig.runOf (late)->error == "pd-held");
    }

    // The loop does end, and Pd is free again: a new instance can be made.
    PdInstance after (settingsFor ("after-stuck"));
    REQUIRE (after.make());
    CHECK (after.finished (60s));
}

//==============================================================================
/*  PC.3: MIDI, what a patch says, the ports readout, process.send, the puck. */

TEST_CASE ("process cue: a note on the port its cue listens on reaches [notein]")
{
    ProcessRig rig ("pc3-notein");
    const auto cue = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                      "#X obj 10 10 notein;\n"
                                      "#X obj 10 40 s /dev/out;\n"
                                      "#X connect 0 0 1 0;\n");
    rig.document.setAttribute ("/godot/cue/" + cue + "/midiIn", "PORTA001");
    rig.fire (cue);
    REQUIRE (rig.running (cue));

    rig.listener.clear();
    rig.midiInbox.push ("PORTB002", { 0x90, 61, 100 });   // another port: not heard
    rig.midiInbox.push ("PORTA001", { 0x90, 60, 100 });
    rig.tickOnce();

    const auto pitch = rig.listener.valueAt ("/dev/out");
    REQUIRE (pitch.has_value());
    CHECK (sameNumber (*pitch, 60.0));
    CHECK (rig.listener.packets().size() == 1u);
}

TEST_CASE ("process cue: [noteout] sends on the port its cue names, as a cue's message")
{
    ProcessRig rig ("pc3-noteout");
    const auto cue = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                      "#X obj 10 10 r /dev/in;\n"
                                      "#X msg 10 40 60 100;\n"
                                      "#X obj 10 70 noteout 2;\n"
                                      "#X connect 0 0 1 0;\n"
                                      "#X connect 1 0 2 0;\n");
    rig.document.setAttribute ("/godot/cue/" + cue + "/midiOut", "PORTA001");
    rig.fire (cue);
    REQUIRE (rig.running (cue));

    rig.hear ("/dev/in", 1.0f);
    rig.tickOnce();

    REQUIRE (rig.midiSink.sent.size() == 1u);
    CHECK (rig.midiSink.sent[0].first == "PORTA001");
    CHECK (rig.midiSink.sent[0].second == wfg::midi::Bytes { 0x91, 60, 100 });
}

TEST_CASE ("process cue: what a patch prints is the run's said line, and its ports their last values")
{
    ProcessRig rig ("pc3-said");
    const auto cue = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                      "#X obj 10 10 r /dev/in;\n"
                                      "#X obj 10 40 * 2;\n"
                                      "#X obj 10 70 s /dev/out;\n"
                                      "#X obj 100 40 print level;\n"
                                      "#X connect 0 0 1 0;\n"
                                      "#X connect 1 0 2 0;\n"
                                      "#X connect 0 0 3 0;\n");
    rig.fire (cue);
    REQUIRE (rig.running (cue));
    rig.hear ("/dev/in", 0.25f);
    rig.tickOnce();

    const auto* run = rig.runOf (cue);
    CHECK (run->processSaid == "level: 0.25");
    CHECK (run->processPorts.find ("/dev/out 0.5\n") != std::string::npos);
    CHECK (run->processPorts.find ("/dev/in 0.25\n") != std::string::npos);
}

TEST_CASE ("process cue: process.send hands atoms to a name the patch hears")
{
    ProcessRig rig ("pc3-send");
    const auto cue = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                      "#X obj 10 10 r /knob;\n"
                                      "#X obj 10 40 s /dev/out;\n"
                                      "#X connect 0 0 1 0;\n");
    rig.fire (cue);
    REQUIRE (rig.running (cue));
    const auto runId = rig.runOf (cue)->id;

    rig.listener.clear();
    rig.submit ("process.send", { wfg::osc::Value::string (runId), wfg::osc::Value::string ("/knob"),
                                  wfg::osc::Value::float64 (0.75) });
    rig.tickOnce();
    const auto sent = rig.listener.valueAt ("/dev/out");
    REQUIRE (sent.has_value());
    CHECK (sameNumber (*sent, 0.75));

    // Refused for a run that is not a running process.
    REQUIRE (rig.engine.submit ("cli", "process.send", { wfg::osc::Value::string ("NONEXIST"),
                                                         wfg::osc::Value::string ("/knob") }));
    rig.tickOnce();
    const auto records = rig.records();
    CHECK (std::any_of (records.begin(), records.end(), [] (const auto& r)
    {
        return r.kind == wfg::LogRecord::Kind::rejected && r.command == "process.send";
    }));
}

TEST_CASE ("process cue: a patch hearing /godot/puck asks for the SpaceMouse and hears its axes")
{
    ProcessRig rig ("pc3-puck");
    const auto cue = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                      "#X obj 10 10 r /godot/puck;\n"
                                      "#X obj 10 40 unpack f f f f f f;\n"
                                      "#X obj 10 70 s /dev/out;\n"
                                      "#X connect 0 0 1 0;\n"
                                      "#X connect 1 2 2 0;\n");
    rig.fire (cue);
    REQUIRE (rig.running (cue));
    rig.tickOnce();
    CHECK (rig.runner.processesWantPuck());

    rig.listener.clear();
    rig.runner.notePuck (std::array<double, 6> { 0.1, 0.2, -0.5, 0.0, 0.0, 0.0 });
    rig.tickOnce();
    const auto z = rig.listener.valueAt ("/dev/out");
    REQUIRE (z.has_value());
    CHECK (sameNumber (*z, -0.5));

    // Not live: nothing heard.
    rig.listener.clear();
    rig.runner.notePuck (std::nullopt);
    rig.tickOnce();
    CHECK_FALSE (rig.listener.valueAt ("/dev/out", 100).has_value());
}
