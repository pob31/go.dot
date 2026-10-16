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

/*  DECKLINK CARDS (namespace draft §71). No card is on any machine that runs
    this suite, CI's or the author's: what reaches a real card is checked by the
    testers' probe (DL.10). These cases hold what can be held without one - the
    SDK's headers, the words a format is stored as, and the TEST CARD (APO),
    which everything after them is tested against. */

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/video/DeckLinkProbe.h>
#include <wfg/engine/video/VideoHost.h>
#include <wfg/engine/video/VideoModes.h>
#include <wfg/engine/video/VideoRegion.h>
#include <wfg/engine/video/render/Card.h>
#include <wfg/engine/video/render/DeckLinkApi.h>

#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <new>
#include <sstream>
#include <thread>

using namespace wfg;

namespace
{
    void waitMs (int ms)
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (ms));
    }

    /*  A frame lent by `output`, filled with one byte, scheduled at `index`. */
    bool sendFrame (video::card::Output& output, std::int64_t index, std::uint8_t fill)
    {
        const auto frame = output.lend();

        if (frame.pixels == nullptr)
            return false;

        std::memset (frame.pixels, fill, static_cast<std::size_t> (frame.rowBytes * output.mode().format.height));
        return output.schedule (frame, index);
    }

    struct Missing final : video::card::System
    {
        video::card::Found find() override
        {
            video::card::Found found;
            found.problem = "Desktop Video is not installed: Blackmagic's drivers, from www.blackmagicdesign.com/support";
            return found;
        }

        std::unique_ptr<video::card::Output> openOutput (const std::string&, const video::card::OutputSettings&, std::string&) override
        {
            return nullptr;
        }

        std::unique_ptr<video::card::Input> openInput (const std::string&, const std::string&, std::string&) override
        {
            return nullptr;
        }
    };
}

TEST_CASE ("decklink: the SDK's headers are 16.0's and reach the library's entry point (DL.0)")
{
    //  The version the headers were carried at (scripts/vendor-decklink.py),
    //  which is the oldest Desktop Video Go.dot asks for (APA).
    CHECK (BLACKMAGIC_DECKLINK_API_VERSION == 0x10000000);
    CHECK (std::strcmp (BLACKMAGIC_DECKLINK_API_VERSION_STRING, "16.0") == 0);

    //  The way to the library links on this system: MIDL's identifiers on
    //  Windows, the dispatch file's loader elsewhere. Nothing is called here.
   #if defined (_WIN32)
    CHECK (CLSID_CDeckLinkIterator.Data1 != 0u);
    CHECK (IID_IDeckLinkOutput.Data1 != 0u);
   #else
    IDeckLinkIterator* (* volatile create)() = CreateDeckLinkIteratorInstance;
    CHECK (create != nullptr);
   #endif
}

TEST_CASE ("decklink: a format is stored as a word made from it, and read back exactly (DL.1)")
{
    using video::modes::Format;

    const auto word = [] (int width, int height, std::int64_t duration, std::int64_t scale, bool interlaced)
    {
        return video::modes::wordFor (Format { width, height, duration, scale, interlaced });
    };

    //  As broadcast names them: interlaced by fields a second.
    CHECK (word (1920, 1080, 1, 50, false) == "1080p50");
    CHECK (word (1920, 1080, 1001, 60000, false) == "1080p59.94");
    CHECK (word (1920, 1080, 1, 25, true) == "1080i50");
    CHECK (word (1920, 1080, 1001, 30000, true) == "1080i59.94");
    CHECK (word (3840, 2160, 1001, 24000, false) == "2160p23.98");
    CHECK (word (2048, 1080, 1, 24, false) == "dci1080p24");
    CHECK (word (1920, 1200, 1, 60, false) == "1920x1200p60");
    CHECK (word (1280, 720, 1001, 120000, false) == "720p119.88");
    CHECK (word (0, 1080, 1, 50, false).empty());

    for (const char* each : { "1080p50", "1080p59.94", "1080i50", "1080i59.94", "2160p23.98", "dci2160p48",
                              "720p50", "1920x1200p60", "720x486i59.94", "4320p119.88" })
    {
        const auto format = video::modes::parse (each);
        REQUIRE_MESSAGE (format.has_value(), each);
        CHECK (video::modes::wordFor (*format) == each);
    }

    const auto i50 = video::modes::parse ("1080i50");
    REQUIRE (i50.has_value());
    CHECK (i50->width == 1920);
    CHECK (i50->interlaced);
    CHECK (std::abs (i50->frameRate() - 25.0) < 1.0e-9);

    const auto p5994 = video::modes::parse ("1080p59.94");
    REQUIRE (p5994.has_value());
    CHECK (p5994->frameDuration == 1001);
    CHECK (p5994->timeScale == 60000);

    //  Only a format's own word is its word.
    for (const char* not_ : { "", "1080", "1080p", "1080p050", "1080p50.00", "1080p59.9", "1080q50", "dci720p50",
                              "x1080p50", "1080p-50", "1080p50 ", "auto" })
        CHECK_MESSAGE (! video::modes::parse (not_).has_value(), not_);

    CHECK (std::string (video::modes::defaultWord) == "1080p50");
}

TEST_CASE ("decklink: the test card is found, with what it can do and its formats (DL.1)")
{
    auto cards = video::card::makeFakeSystem();
    const auto found = cards->find();

    CHECK (found.problem.empty());
    REQUIRE (found.cards.size() == 1);

    const auto& card = found.cards.front();
    CHECK (card.id == "fake-1");
    CHECK (card.name == "Go.dot test card");
    CHECK (card.output);
    CHECK (card.input);
    CHECK (card.internalKey);
    CHECK (card.externalKey);
    CHECK (card.tenBit);
    CHECK (card.audioChannels == 16);

    bool p50 = false;

    for (const auto& mode : card.modes)
    {
        CHECK (video::modes::wordFor (mode.format) == mode.word);
        p50 = p50 || (mode.word == "1080p50" && mode.output && mode.input);
    }

    CHECK (p50);

    //  Unplugged: no card, and nothing wrong with Desktop Video.
    video::card::FakeOptions absent;
    absent.present = false;
    const auto none = video::card::makeFakeSystem (absent)->find();
    CHECK (none.cards.empty());
    CHECK (none.problem.empty());
}

TEST_CASE ("decklink: the probe lists each card for a tester, and says in words when Desktop Video is missing (DL.1)")
{
    {
        auto cards = video::card::makeFakeSystem();
        std::ostringstream out;
        CHECK (video::runDeckLinkProbe (*cards, {}, out) == 0);

        const auto said = out.str();
        CHECK (said.find ("Go.dot test card  (fake-1)") != std::string::npos);
        CHECK (said.find ("sends pictures: yes") != std::string::npos);
        CHECK (said.find ("keyers: internal and external") != std::string::npos);
        CHECK (said.find ("formats it sends: 1080p50 1080p25 1080p59.94") != std::string::npos);
    }

    {
        Missing missing;
        std::ostringstream out;
        CHECK (video::runDeckLinkProbe (missing, {}, out) == 2);
        CHECK (out.str().find ("Desktop Video is not installed") != std::string::npos);
    }

    //  This machine's own Desktop Video, as it is: on CI and the author's
    //  box there is none, and it says so; where there is, it answers.
    auto real = video::card::makeDeckLinkSystem();
    const auto found = real->find();

    if (found.problem.empty())
        MESSAGE ("Desktop Video " << found.version << " here, " << found.cards.size() << " card(s)");
    else
        CHECK (found.problem.find ("Desktop Video") != std::string::npos);
}

TEST_CASE ("decklink: the test card sends what was scheduled at its frame's moment, and counts what was late (DL.1)")
{
    auto cards = video::card::makeFakeSystem();
    std::string why;

    video::card::OutputSettings settings;
    auto output = cards->openOutput ("fake-1", settings, why);
    REQUIRE (output != nullptr);
    CHECK (output->mode().word == "1080p50");
    CHECK (output->format() == video::card::PixelFormat::yuv10);

    //  One output to a card: a second is another program's.
    std::string busy;
    CHECK (cards->openOutput ("fake-1", settings, busy) == nullptr);
    CHECK (busy == "another program has the card");

    std::string noSuch;
    CHECK (cards->openOutput ("fake-9", settings, noSuch) == nullptr);
    CHECK (noSuch == "the card is not connected");

    settings.mode = "1080p48";
    std::string notOffered;
    CHECK (cards->openOutput ("fake-1", settings, notOffered) == nullptr);
    CHECK (notOffered == "the card cannot send 1080p48");

    //  Two frames ahead, then sending: frame 0 leaves at 20 ms, frame 1 at 40.
    REQUIRE (sendFrame (*output, 0, 0x11));
    REQUIRE (sendFrame (*output, 1, 0x22));
    CHECK (output->waiting() == 2);
    REQUIRE (output->start (0));
    waitMs (120);

    auto counted = output->completions();
    CHECK (counted.shown == 2);
    CHECK (counted.late == 0);
    CHECK (output->waiting() == 0);

    const auto sent = video::card::monitorOf (*output);
    CHECK (sent.index == 1);
    CHECK (sent.rowBytes == video::card::rowBytesFor (video::card::PixelFormat::yuv10, 1920));
    REQUIRE (! sent.pixels.empty());
    CHECK (sent.pixels.front() == 0x22);

    //  Frame 2's moment went by while nothing was scheduled: scheduled now, it
    //  is dropped - never sent. One for a moment still to come is sent.
    REQUIRE (sendFrame (*output, 2, 0x33));
    std::int64_t streamNanos = 0, nanos = 0;
    REQUIRE (output->clock (streamNanos, nanos));
    const auto& format = output->mode().format;
    const auto next = streamNanos * format.timeScale / (1'000'000'000 * format.frameDuration) + 3;
    REQUIRE (sendFrame (*output, next, 0x44));
    waitMs (140);

    counted = output->completions();
    CHECK (counted.dropped == 1);
    CHECK (counted.shown == 3);
    CHECK (video::card::monitorOf (*output).pixels.front() == 0x44);
}

TEST_CASE ("decklink: the test card's clock runs as fast as it was told to drift (DL.1)")
{
    video::card::FakeOptions options;
    options.driftPpm = 1000.0;   // a tenth of a percent fast
    auto cards = video::card::makeFakeSystem (options);
    std::string why;
    auto output = cards->openOutput ("fake-1", {}, why);
    REQUIRE (output != nullptr);
    REQUIRE (output->start (0));

    std::int64_t time0 = 0, nanos0 = 0, time1 = 0, nanos1 = 0;
    REQUIRE (output->clock (time0, nanos0));
    waitMs (300);
    REQUIRE (output->clock (time1, nanos1));

    //  The stream's nanoseconds against the machine's.
    const double ppm = (static_cast<double> (time1 - time0) / static_cast<double> (nanos1 - nanos0) - 1.0) * 1.0e6;
    CHECK (std::abs (ppm - 1000.0) < 20.0);
}

TEST_CASE ("decklink: a card output's and input's settings cross to the renderer whole, and its kind is not read as another (DL.2)")
{
    std::vector<char> bytes (video::region::regionBytes(), 0);
    auto* r = new (bytes.data()) video::region::Region {};
    video::region::stamp (*r);

    video::region::OutputReading output;
    output.id = "MC000070";
    output.kind = video::region::OutputKind::decklink;
    output.card = "dl-1a2b3c4d";
    output.videoMode = "2160p59.94";
    output.key = video::region::CardKey::internal;
    output.sound = { 0, 1, -1, 5 };

    video::region::InputReading input;
    input.id = "MC000071";
    input.kind = video::region::OutputKind::decklink;
    input.sender = "dl-1a2b3c4d";
    input.videoMode = "1080i50";

    video::region::writeConfig (*r, {}, { output }, { input });

    video::region::ConfigReading read;
    REQUIRE (video::region::readConfig (*r, read));
    REQUIRE (read.outputs.size() == 1);
    CHECK (read.outputs[0].kind == video::region::OutputKind::decklink);
    CHECK (read.outputs[0].toCard());
    CHECK_FALSE (read.outputs[0].sends());
    CHECK (read.outputs[0].card == "dl-1a2b3c4d");
    CHECK (read.outputs[0].videoMode == "2160p59.94");
    CHECK (read.outputs[0].key == video::region::CardKey::internal);
    CHECK (read.outputs[0].sound == std::vector<int> { 0, 1, -1, 5 });

    REQUIRE (read.inputs.size() == 1);
    CHECK (read.inputs[0].kind == video::region::OutputKind::decklink);
    CHECK (read.inputs[0].videoMode == "1080i50");

    //  The cards found, and a card output's state, the renderer's side.
    video::region::writeCards (*r, "fake-1\tGo.dot test card\toutput input\t1080p50\t1080p50");
    CHECK (video::region::readCards (*r) == "fake-1\tGo.dot test card\toutput input\t1080p50\t1080p50");
    video::region::writeCardState (*r, 0, 40.0, -12.5, 1, 3);
    CHECK (r->outputs[0].latencyMs.load() == doctest::Approx (40.0));
    CHECK (r->outputs[0].skewPpm.load() == doctest::Approx (-12.5));
    CHECK (r->outputs[0].reference.load() == 1u);
    CHECK (r->outputs[0].soundLate.load() == 3u);

    //  A kind written past the last read back as the last, never as a smaller one.
    CHECK (video::region::storedKind (99u) == video::region::OutputKind::decklink);
    CHECK (video::region::outputKindFrom ("decklink") == video::region::OutputKind::decklink);
}

TEST_CASE ("decklink: what videoOutput/cards says of the test card, and of Desktop Video missing (DL.2)")
{
    auto cards = video::card::makeFakeSystem();
    const auto said = video::card::describe (cards->find());
    CHECK (said.rfind ("fake-1\tGo.dot test card\toutput input internal external 10bit sound16\t1080p50 1080p25", 0) == 0);
    CHECK (said.find ('\n') == std::string::npos);

    Missing missing;
    CHECK (video::card::describe (missing.find()).rfind ("problem\tDesktop Video is not installed", 0) == 0);

    video::card::FakeOptions absent;
    absent.present = false;
    CHECK (video::card::describe (video::card::makeFakeSystem (absent)->find()).empty());
}

namespace
{
    /*  The renderer is a child process and takes the environment it is
        started with: a test card, and the software rasteriser on Windows. */
    void setVariable (const char* name, const char* value)
    {
       #if defined (_WIN32)
        SetEnvironmentVariableA (name, value);
       #else
        if (value != nullptr)
            setenv (name, value, 1);
        else
            unsetenv (name);
       #endif
    }

    /*  Go.dot's samples as the machine's time goes by, at 48 kHz, as the
        tick writes them. */
    struct Ticks
    {
        std::int64_t now() const
        {
            const auto elapsed = std::chrono::steady_clock::now() - began;
            return std::chrono::duration_cast<std::chrono::microseconds> (elapsed).count() * 48 / 1000;
        }

        std::chrono::steady_clock::time_point began = std::chrono::steady_clock::now();
    };

    template <typename Done>
    bool tickUntil (video::VideoHost& host, const Ticks& ticks, Done done, int milliseconds = 20000)
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds (milliseconds);

        while (std::chrono::steady_clock::now() < until)
        {
            host.tick (ticks.now(), 48000);

            if (done())
                return true;

            waitMs (20);
        }

        return false;
    }
}

TEST_CASE ("video gpu: a card output on the test card is paced by the card, and says its latency, its skew and its frames (§71, DL.3)")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-card-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    const juce::File bundle { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/video" };
    REQUIRE (doc::Bundle::open (bundle, document).ok);

    //  The fixture's output, sent down the test card's cable at 720p50.
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/kind", "decklink").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/card", "fake-1").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/videoMode", "720p50").ok);

    //  A card whose clock runs a thousandth fast.
    setVariable ("WFG_DECKLINK_FAKE", "1");
    setVariable ("WFG_DECKLINK_FAKE_DRIFT", "1000");
    setVariable ("WFG_VIDEO_SOFTWARE", "1");

    Ticks ticks;
    video::VideoHost host { spec };
    host.configure (document);

    const auto running = tickUntil (host, ticks, [&host] { return host.readouts().renderer == "running"; });
    setVariable ("WFG_DECKLINK_FAKE", nullptr);
    setVariable ("WFG_DECKLINK_FAKE_DRIFT", nullptr);
    setVariable ("WFG_VIDEO_SOFTWARE", nullptr);
    REQUIRE (running);

    //  The card found, and published as `videoOutput/cards` says it.
    REQUIRE (tickUntil (host, ticks, [&host] { return host.readouts().cards.rfind ("fake-1\t", 0) == 0; }, 5000));

    //  Frames reach the card on its own clock (APB): bound, sent, counted.
    REQUIRE (tickUntil (host, ticks, [&host]
                        {
                            const auto said = host.readouts();
                            const auto* output = said.output ("VD000021");
                            return output != nullptr && output->bound && output->framesPresented > 10;
                        }, 15000));

    const auto first = host.readouts().output ("VD000021")->framesPresented;
    const auto from = std::chrono::steady_clock::now();
    tickUntil (host, ticks, [] { return false; }, 2000);
    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - from).count();

    const auto said = host.readouts();
    const auto* output = said.output ("VD000021");
    REQUIRE (output != nullptr);

    const auto rate = static_cast<double> (output->framesPresented - first) / seconds;
    MESSAGE ("card frames a second: " << rate << ", late or dropped: " << output->framesLate << ", latency " << output->latencyMs
             << " ms, skew " << output->skewPpm << " ppm");

    //  The card asks fifty a second; a software rasteriser in a Debug build
    //  keeps up with most of them, and what it misses is counted, not hidden.
    CHECK (rate > 20.0);
    CHECK (rate < 52.0);
    CHECK (output->problem.empty());

    //  Two frames waiting at most (APC): up to 40 ms.
    CHECK (output->latencyMs >= 0.0);
    CHECK (output->latencyMs <= 40.5);

    //  The card's thousandth against the sound's clock, shown (§3.19d).
    CHECK (output->skewPpm == doctest::Approx (1000.0).epsilon (0.3));
    CHECK (output->reference == "none");

    //  A card that is not there says so, in words, and is tried again.
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/card", "fake-9").ok);
    host.configure (document);

    CHECK (tickUntil (host, ticks, [&host]
                      {
                          const auto latest = host.readouts();
                          const auto* now = latest.output ("VD000021");
                          return now != nullptr && ! now->bound && now->problem == "the card is not connected";
                      }, 10000));
}
