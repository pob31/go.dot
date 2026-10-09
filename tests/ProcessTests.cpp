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
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/DocumentCommands.h>
#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/log/EventLog.h>
#include <wfg/engine/midi/MidiSink.h>
#include <wfg/engine/osc/OscCodec.h>
#include <wfg/engine/osc/UdpEndpoint.h>
#include <wfg/engine/process/PatchEditor.h>
#include <wfg/engine/process/PatchText.h>
#include <wfg/engine/process/PdInstance.h>
#include <wfg/engine/process/ProcessHost.h>
#include <wfg/engine/serial/SerialTable.h>
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
#include <fstream>
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

//==============================================================================
/*  PC.7: A PATCH OPENED IN PLUGDATA OR PD, as a program of its own; each save
    noticed and handed back. No program is started here: the watch is given
    none, and the commands nothing to open. */

TEST_CASE ("process editor: a patch written out for editing, and each save there noticed once")
{
    const auto folder = scratch ("pc7-watch");
    std::filesystem::remove_all (folder);
    wfg::process::editor::Watch watch (folder.string());

    const std::string patch = "#N canvas 0 50 450 300 12;\n#X obj 10 10 r /a;\n";
    CHECK (watch.open ({}, "PRCS0001", patch).empty());
    CHECK (watch.watching() == 1u);

    const auto file = std::filesystem::path (watch.fileFor ("PRCS0001"));
    REQUIRE (std::filesystem::exists (file));
    CHECK (watch.saved().empty());

    //  A save: new words, and a later time on the file.
    const std::string saved = "#N canvas 0 50 450 300 12;\n#X obj 10 10 r /b;\n";
    {
        std::ofstream out (file, std::ios::binary | std::ios::trunc);
        out << saved;
    }
    std::filesystem::last_write_time (file, std::filesystem::last_write_time (file) + std::chrono::seconds (2));

    const auto changes = watch.saved();
    REQUIRE (changes.size() == 1u);
    CHECK (changes[0].first == "PRCS0001");
    CHECK (changes[0].second == saved);
    CHECK (watch.saved().empty());

    //  A save of the same words is not a change.
    std::filesystem::last_write_time (file, std::filesystem::last_write_time (file) + std::chrono::seconds (2));
    CHECK (watch.saved().empty());
}

TEST_CASE ("process editor: process.edit is refused on what is not a process cue and under the lock, taken in a replay")
{
    ProcessRig rig ("pc7-edit", false);
    wfg::process::editor::registerCommands (rig.engine.commands(), rig.document, nullptr, nullptr);

    const auto cue = rig.makeProcess (doubling());
    const auto memo = rig.makeMemo ("Not a patch");

    const auto refusedWith = [&] (const std::string& id) -> std::string
    {
        REQUIRE (rig.engine.submit ("cli", "process.edit", { wfg::osc::Value::string (id) }));
        rig.tickOnce();
        const auto records = rig.records();
        for (auto at = records.rbegin(); at != records.rend(); ++at)
            if (at->command == "process.edit")
                return at->kind == wfg::LogRecord::Kind::rejected ? at->reason : std::string ("applied");
        return "missing";
    };

    CHECK (refusedWith (memo) == "unknown-id");
    CHECK (refusedWith (cue) == "applied");

    REQUIRE (rig.document.setAttribute ("/godot/document/locked", "true").ok);
    CHECK (refusedWith (cue) == "locked");

    //  pd.install with nothing to download: taken and ignored.
    REQUIRE (rig.engine.submit ("cli", "pd.install", {}));
    rig.tickOnce();
}

//==============================================================================
/*  PC.9: GO.DOT'S READY-MADE PATCHES (namespace draft §51, ACU), each played
    through a patch that uses it - [r /in] and [r /right] into its inlets, a
    `reset` from [r /reset], its outlets to /out and /out2 - a value a tick,
    and every help patch opened with nothing printed against it. A tick is
    20 ms of Pd's time. */
namespace
{
    std::filesystem::path readyMadeFolder()
    {
        return std::filesystem::path (std::string (WFG_REPO_ROOT)) / "pd";
    }

    PdInstance::Settings withReadyMade (const std::string& name)
    {
        auto settings = settingsFor (name);
        settings.searchPaths.push_back (readyMadeFolder().string());
        return settings;
    }

    std::string around (const std::string& box, int inlets, int outlets)
    {
        std::string text = "#N canvas 0 0 450 300 12;\n"
                           "#X obj 10 10 r /in;\n"
                           "#X obj 200 10 r /right;\n"
                           "#X obj 300 10 r /reset;\n"
                           "#X msg 300 40 reset;\n"
                           "#X obj 10 80 " + box + ";\n"
                           "#X obj 10 140 s /out;\n"
                           "#X obj 200 140 s /out2;\n"
                           "#X connect 0 0 4 0;\n"
                           "#X connect 2 0 3 0;\n"
                           "#X connect 3 0 4 0;\n"
                           "#X connect 4 0 5 0;\n";
        if (inlets > 1)
            text += "#X connect 1 0 4 1;\n";
        if (outlets > 1)
            text += "#X connect 4 1 6 0;\n";
        return text;
    }

    /*  What Pd printed that says something did not work. */
    std::vector<std::string> troubleIn (const Outbox& outbox)
    {
        std::vector<std::string> out;
        for (const auto& line : outbox.printed)
            for (const char* word : { "error", "couldn't", "no method", "failed", "bad ", "unknown", "can't" })
                if (line.find (word) != std::string::npos)
                {
                    out.push_back (line);
                    break;
                }
        return out;
    }

    std::string joined (const std::vector<std::string>& lines)
    {
        std::string out;
        for (const auto& line : lines)
            out += line + "\n";
        return out;
    }

    int bangsTo (const Outbox& outbox, const std::string& to)
    {
        return static_cast<int> (std::count_if (outbox.sent.begin(), outbox.sent.end(),
                                                [&to] (const Sent& s) { return s.to == to && s.selector == "bang"; }));
    }

    std::vector<std::string> wordsSentTo (const Outbox& outbox, const std::string& to)
    {
        std::vector<std::string> out;
        for (const auto& sent : outbox.sent)
            if (sent.to == to)
                for (const auto& atom : sent.atoms)
                    if (! atom.isNumber)
                        out.push_back (atom.word);
        return out;
    }

    struct Played
    {
        PdInstance pd;

        Played (const std::string& name, const std::string& box, int inlets, int outlets)
            : pd (withReadyMade (name))
        {
            makeAndOpen (pd, around (box, inlets, outlets));
            const auto opened = pd.takeOutbox();
            INFO (joined (opened.printed));
            CHECK (troubleIn (opened).empty());
        }

        Outbox tick (std::vector<Input> inputs = {}) { return tickOnce (pd, std::move (inputs)); }
        Outbox in (double value) { return tick ({ { "/in", { Atom::of (value) } } }); }
        Outbox in (Atoms atoms) { return tick ({ { "/in", std::move (atoms) } }); }
        Outbox right (double value) { return tick ({ { "/right", { Atom::of (value) } } }); }
        Outbox reset() { return tick ({ { "/reset", {} } }); }

        /*  The one number a value in sends to /out, or nothing. */
        std::optional<double> out (double value)
        {
            const auto sent = numbersSentTo (in (value), "/out");
            if (sent.empty())
                return std::nullopt;
            CHECK (sent.size() == 1u);
            return sent.front();
        }
    };

    bool is (std::optional<double> got, double expected)
    {
        return got.has_value() && sameNumber (*got, expected);
    }
}

TEST_CASE ("ready-made: go.avg - the average of the last values, forgotten on reset, a new window from the right")
{
    Played avg ("go-avg", "go.avg 4", 2, 1);
    CHECK (is (avg.out (1.0), 1.0));
    CHECK (is (avg.out (2.0), 1.5));
    CHECK (is (avg.out (3.0), 2.0));
    CHECK (is (avg.out (4.0), 2.5));
    CHECK (is (avg.out (5.0), 3.5));      // 1 has left the window of four
    CHECK (is (avg.out (6.0), 4.5));

    avg.reset();
    CHECK (is (avg.out (10.0), 10.0));

    CHECK (numbersSentTo (avg.right (2.0), "/out").empty());
    CHECK (is (avg.out (4.0), 4.0));
    CHECK (is (avg.out (6.0), 5.0));
    CHECK (is (avg.out (8.0), 7.0));
}

TEST_CASE ("ready-made: go.avg in ms - the values that came in the window's time")
{
    Played avg ("go-avg-ms", "go.avg 100 ms", 2, 1);
    CHECK (is (avg.out (10.0), 10.0));    // at 0 ms
    CHECK (is (avg.out (20.0), 15.0));    // at 20
    CHECK (is (avg.out (30.0), 20.0));    // at 40
    for (int tick = 3; tick < 7; ++tick)
        CHECK (numbersSentTo (avg.tick(), "/out").empty());
    CHECK (is (avg.out (40.0), 35.0));    // at 140: what came at 0 and 20 is older than 100 ms
    for (int tick = 8; tick < 20; ++tick)
        avg.tick();
    CHECK (is (avg.out (50.0), 50.0));    // at 400: alone
}

TEST_CASE ("ready-made: go.minmax - the smallest and the largest of the last values")
{
    Played minmax ("go-minmax", "go.minmax 3", 2, 2);
    const auto both = [&minmax] (double value)
    {
        const auto outbox = minmax.in (value);
        const auto low = numbersSentTo (outbox, "/out");
        const auto high = numbersSentTo (outbox, "/out2");
        REQUIRE (low.size() == 1u);
        REQUIRE (high.size() == 1u);
        return std::pair { low[0], high[0] };
    };
    CHECK (both (5.0) == std::pair { 5.0, 5.0 });
    CHECK (both (1.0) == std::pair { 1.0, 5.0 });
    CHECK (both (9.0) == std::pair { 1.0, 9.0 });
    CHECK (both (7.0) == std::pair { 1.0, 9.0 });
    CHECK (both (8.0) == std::pair { 7.0, 9.0 });    // 5 and 1 have left
    minmax.reset();
    CHECK (both (-3.0) == std::pair { -3.0, -3.0 });
}

TEST_CASE ("ready-made: go.minmax in ms - over the window's time")
{
    Played minmax ("go-minmax-ms", "go.minmax 60 ms", 2, 2);
    CHECK (numbersSentTo (minmax.in (9.0), "/out2") == std::vector<double> { 9.0 });   // at 0
    minmax.in (2.0);                                                                      // at 20
    for (int tick = 2; tick < 4; ++tick)
        minmax.tick();
    const auto outbox = minmax.in (5.0);                                                  // at 80: 9 is gone
    CHECK (numbersSentTo (outbox, "/out") == std::vector<double> { 2.0 });
    CHECK (numbersSentTo (outbox, "/out2") == std::vector<double> { 5.0 });
}

TEST_CASE ("ready-made: go.smooth - each value part of the way, the first whole")
{
    Played smooth ("go-smooth", "go.smooth 0.5", 2, 1);
    CHECK (is (smooth.out (10.0), 10.0));
    CHECK (is (smooth.out (20.0), 15.0));
    CHECK (is (smooth.out (20.0), 17.5));
    smooth.reset();
    CHECK (is (smooth.out (4.0), 4.0));
    smooth.right (0.0);
    CHECK (is (smooth.out (8.0), 8.0));

    Played byDefault ("go-smooth-default", "go.smooth", 2, 1);
    CHECK (is (byDefault.out (0.0), 0.0));
    CHECK (is (byDefault.out (100.0), 25.0));       // three quarters kept
}

TEST_CASE ("ready-made: go.scale - one range onto another, clipped when asked, changed by message")
{
    Played scale ("go-scale", "go.scale 0 10 100 200", 1, 1);
    CHECK (is (scale.out (5.0), 150.0));
    CHECK (is (scale.out (20.0), 300.0));
    CHECK (numbersSentTo (scale.in ({ Atom::of (std::string ("clip")), Atom::of (1.0) }), "/out").empty());
    CHECK (is (scale.out (20.0), 200.0));
    scale.in ({ Atom::of (std::string ("out")), Atom::of (1.0), Atom::of (0.0) });
    CHECK (is (scale.out (5.0), 0.5));               // upside down
    scale.in ({ Atom::of (std::string ("in")), Atom::of (0.0), Atom::of (100.0) });
    CHECK (is (scale.out (25.0), 0.75));

    Played plain ("go-scale-plain", "go.scale", 1, 1);
    CHECK (is (plain.out (0.25), 0.25));

    Played clipped ("go-scale-clip", "go.scale 0 1 -90 90 clip", 1, 1);
    CHECK (is (clipped.out (2.0), 90.0));
    CHECK (is (clipped.out (0.5), 0.0));
}

TEST_CASE ("ready-made: go.deadband - a value passes only once it has moved past the band")
{
    Played band ("go-deadband", "go.deadband 4", 2, 1);
    CHECK (is (band.out (100.0), 100.0));
    CHECK_FALSE (band.out (102.0).has_value());
    CHECK (is (band.out (105.0), 105.0));
    CHECK_FALSE (band.out (101.0).has_value());     // four is not past a band of four
    CHECK (is (band.out (100.0), 100.0));
    band.reset();
    CHECK (is (band.out (100.0), 100.0));            // the first after reset
    band.right (0.0);
    CHECK_FALSE (band.out (100.0).has_value());
    CHECK (is (band.out (101.0), 101.0));
}

TEST_CASE ("ready-made: go.change - a number or a word that is not the last one")
{
    Played change ("go-change", "go.change", 2, 1);
    CHECK (is (change.out (3.0), 3.0));
    CHECK_FALSE (change.out (3.0).has_value());
    CHECK (is (change.out (4.0), 4.0));

    const auto word = [&change] (const char* w) { return wordsSentTo (change.in ({ Atom::of (std::string (w)) }), "/out"); };
    CHECK (word ("open") == std::vector<std::string> { "open" });
    CHECK (word ("open").empty());
    CHECK (word ("shut") == std::vector<std::string> { "shut" });

    change.reset();
    CHECK (is (change.out (4.0), 4.0));
    CHECK (word ("shut") == std::vector<std::string> { "shut" });

    change.tick ({ { "/right", {} } });               // a bang on the right forgets too
    CHECK (is (change.out (4.0), 4.0));
}

TEST_CASE ("ready-made: go.edge - a bang going up past the band, another going down")
{
    Played edge ("go-edge", "go.edge 50 10", 2, 2);
    const auto crossed = [&edge] (double value)
    {
        const auto outbox = edge.in (value);
        return std::pair { bangsTo (outbox, "/out"), bangsTo (outbox, "/out2") };
    };
    CHECK (crossed (40.0) == std::pair { 0, 0 });    // the first says where it is
    CHECK (crossed (52.0) == std::pair { 0, 0 });    // inside the band
    CHECK (crossed (60.0) == std::pair { 1, 0 });
    CHECK (crossed (70.0) == std::pair { 0, 0 });
    CHECK (crossed (50.0) == std::pair { 0, 0 });
    CHECK (crossed (44.0) == std::pair { 0, 1 });
    CHECK (crossed (30.0) == std::pair { 0, 0 });
    edge.reset();
    CHECK (crossed (60.0) == std::pair { 0, 0 });
    CHECK (crossed (40.0) == std::pair { 0, 1 });
    edge.right (20.0);                                // a new threshold
    CHECK (crossed (30.0) == std::pair { 1, 0 });
}

TEST_CASE ("ready-made: go.hold - a value held for its time, then the resting value")
{
    Played hold ("go-hold", "go.hold 100", 2, 1);
    std::vector<std::pair<int, double>> seen;
    const auto at = [&] (int tick, Outbox outbox)
    {
        for (const auto value : numbersSentTo (outbox, "/out"))
            seen.emplace_back (tick, value);
    };
    for (int tick = 0; tick < 30; ++tick)
    {
        if (tick == 0 || tick == 8 || tick == 10 || tick == 20)
            at (tick, hold.in (1.0));
        else if (tick == 22)
            at (tick, hold.in (0.0));                 // the resting value: it passes, and the hold ends
        else
            at (tick, hold.tick());
    }
    const std::vector<std::pair<int, double>> expected {
        { 0, 1.0 }, { 5, 0.0 },                       // 100 ms after 0
        { 8, 1.0 }, { 10, 1.0 }, { 15, 0.0 },         // held again from 200 ms
        { 20, 1.0 }, { 22, 0.0 } };
    CHECK (seen == expected);
}

TEST_CASE ("ready-made: go.ratelimit - one message every so often, the newest of those that waited")
{
    Played limit ("go-ratelimit", "go.ratelimit 100", 2, 1);
    std::vector<std::pair<int, std::vector<double>>> seen;
    for (int tick = 0; tick < 25; ++tick)
    {
        Outbox outbox;
        if (tick <= 2)
            outbox = limit.in (static_cast<double> (tick + 1));
        else if (tick == 12)
            outbox = limit.in (4.0);
        else if (tick == 13)
            outbox = limit.in ({ Atom::of (5.0), Atom::of (6.0) });
        else
            outbox = limit.tick();
        if (const auto sent = numbersSentTo (outbox, "/out"); ! sent.empty())
            seen.emplace_back (tick, sent);
    }
    const std::vector<std::pair<int, std::vector<double>>> expected {
        { 0, { 1.0 } },                                // the first at once
        { 5, { 3.0 } },                                // 2 was overtaken by 3
        { 12, { 4.0 } },                               // idle again since 200 ms
        { 17, { 5.0, 6.0 } } };                        // a list, 100 ms after 4
    CHECK (seen == expected);
}

TEST_CASE ("ready-made: every help patch opens with nothing printed against it")
{
    int opened = 0;
    for (const auto& entry : std::filesystem::directory_iterator (readyMadeFolder()))
    {
        const auto file = entry.path().filename().string();
        if (file.size() < 8 || file.substr (file.size() - 8) != "-help.pd")
            continue;

        INFO (file);
        std::ifstream in (entry.path(), std::ios::binary);
        const std::string text ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
        CHECK (parsePatch (text).problem.empty());

        PdInstance pd (withReadyMade ("help-" + file.substr (0, file.size() - 3)));
        makeAndOpen (pd, text);
        const auto outbox = pd.takeOutbox();
        INFO (joined (outbox.printed));
        CHECK (troubleIn (outbox).empty());
        ++opened;
    }
    CHECK (opened == 9);
}

TEST_CASE ("ready-made: the example show opens, and its two patches play with the ready-made ones found")
{
    const auto bundle = juce::File (juce::String (std::string (WFG_REPO_ROOT))).getChildFile ("packaging/Examples/Process examples");
    wfg::doc::ShowDocument document;
    REQUIRE (wfg::doc::Bundle::open (bundle, document).ok);

    const auto patchOf = [&document] (const char* id)
    {
        const auto patch = document.getAttribute (std::string ("/godot/cue/") + id + "/patch");
        REQUIRE (patch.has_value());
        return *patch;
    };

    //  SENSOR TO CUES: a light coming up fires Lights up once, going down Lights down once.
    PdInstance sensor (withReadyMade ("example-sensor"));
    makeAndOpen (sensor, patchOf ("PXSENS0R"));
    {
        const auto opened = sensor.takeOutbox();
        INFO (joined (opened.printed));
        CHECK (troubleIn (opened).empty());
    }
    std::vector<std::string> fired;
    const auto light = [&] (double value)
    {
        for (const auto& sent : tickOnce (sensor, { { "/sensor/light", { Atom::of (value) } } }).sent)
            if (sent.to == "/godot/cmd/cue/fire")
                fired.push_back (sent.selector);
    };
    light (0.0);
    for (int tick = 0; tick < 10; ++tick)
        light (1000.0);
    CHECK (fired == std::vector<std::string> { "PXSHN001" });
    for (int tick = 0; tick < 10; ++tick)
        light (0.0);
    CHECK (fired == std::vector<std::string> { "PXSHN001", "PXDARK01" });

    //  SLIDER TO DESK: the slider's 64 goes to the desk as 64/127.
    PdInstance slider (withReadyMade ("example-slider"));
    makeAndOpen (slider, patchOf ("PXFADER1"));
    {
        const auto opened = slider.takeOutbox();
        INFO (joined (opened.printed));
        CHECK (troubleIn (opened).empty());
    }
    const auto desk = numbersSentTo (tickOnce (slider, { { "/slider", { Atom::of (64.0) } } }), "/desk/fader/1");
    REQUIRE (desk.size() == 1u);
    CHECK (std::abs (desk[0] - 64.0 / 127.0) < 1e-5);
}

//==============================================================================
/*  PC.10: A SERIAL PORT IN A PATCH - a line heard as a `serial.heard` record
    reaches [r /godot/serial/<id>/in] the next tick, and a line a patch sends
    to [s /godot/serial/<id>/out] reaches the port. The port is a fake: what
    is written to it is kept, and it says nothing of its own. */
namespace
{
    struct WrittenDown final : wfg::serial::Link
    {
        explicit WrittenDown (std::shared_ptr<std::pair<std::mutex, std::string>> into) : kept (std::move (into)) {}

        std::optional<std::string> read (std::chrono::milliseconds) override
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
            return std::string {};
        }

        bool write (const std::string& bytes) override
        {
            const std::lock_guard<std::mutex> held (kept->first);
            kept->second += bytes;
            return true;
        }

        std::string problem() const override { return {}; }

        std::shared_ptr<std::pair<std::mutex, std::string>> kept;
    };
}

TEST_CASE ("serial port: a line heard reaches the patch the next tick, and a line it sends reaches the port")
{
    ProcessRig rig ("pc10-serial");

    auto written = std::make_shared<std::pair<std::mutex, std::string>>();
    wfg::serial::SerialTable ports ([written] (const std::string&, int, std::string&) -> std::unique_ptr<wfg::serial::Link>
    {
        return std::make_unique<WrittenDown> (written);
    });
    wfg::serial::Wanted arduino;
    arduino.id = "SR000001";
    arduino.path = "COM3";
    ports.reconcile ({ arduino });
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (ports.stateOf ("SR000001").state != "open" && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for (2ms);
    REQUIRE (ports.stateOf ("SR000001").state == "open");
    rig.runner.setSerialPorts (&ports);

    const auto cue = rig.makeProcess ("#N canvas 0 0 450 300 12;\n"
                                      "#X obj 10 10 r /godot/serial/SR000001/in;\n"
                                      "#X obj 10 40 * 2;\n"
                                      "#X obj 10 70 s /dev/out;\n"
                                      "#X msg 100 40 led \\$1;\n"
                                      "#X obj 100 70 s /godot/serial/SR000001/out;\n"
                                      "#X connect 0 0 1 0;\n"
                                      "#X connect 1 0 2 0;\n"
                                      "#X connect 0 0 3 0;\n"
                                      "#X connect 3 0 4 0;\n");
    rig.fire (cue);
    REQUIRE (rig.running (cue));

    rig.listener.clear();
    rig.submit ("serial.heard", { wfg::osc::Value::string ("SR000001"), wfg::osc::Value::string ("21") });
    CHECK (rig.runner.heardLines().lastLine ("SR000001") == "21");
    rig.tickOnce();

    const auto doubled = rig.listener.valueAt ("/dev/out");
    REQUIRE (doubled.has_value());
    CHECK (sameNumber (*doubled, 42.0));

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    std::string seen;
    while (std::chrono::steady_clock::now() < deadline)
    {
        {
            const std::lock_guard<std::mutex> held (written->first);
            seen = written->second;
        }
        if (! seen.empty())
            break;
        std::this_thread::sleep_for (2ms);
    }
    CHECK (seen == "led 21\n");

    //  Every line, not only a changed one: the same line twice is heard twice.
    rig.listener.clear();
    rig.submit ("serial.heard", { wfg::osc::Value::string ("SR000001"), wfg::osc::Value::string ("21") });
    rig.tickOnce();
    CHECK (rig.listener.valueAt ("/dev/out").has_value());

    //  In the log as the port said it.
    const auto records = rig.records();
    CHECK (std::count_if (records.begin(), records.end(), [] (const wfg::LogRecord& r)
                          { return r.command == "serial.heard" && r.kind == wfg::LogRecord::Kind::applied; }) == 2);
}
