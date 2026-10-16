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

#include <wfg/engine/audio/CardSound.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/video/VideoProbe.h>
#include <wfg/engine/video/VideoHost.h>
#include <wfg/engine/video/VideoModes.h>
#include <wfg/engine/video/VideoRegion.h>
#include <wfg/engine/video/render/Camera.h>
#include <wfg/engine/video/render/Card.h>
#include <wfg/engine/video/render/DeckLinkApi.h>
#include <wfg/engine/video/render/SoundPlacer.h>

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
    const auto linked = create != nullptr;
    CHECK (linked);
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
        CHECK (video::runVideoProbe (*cards, {}, out) == 0);

        const auto said = out.str();
        CHECK (said.find ("Go.dot test card  (fake-1)") != std::string::npos);
        CHECK (said.find ("sends pictures: yes") != std::string::npos);
        CHECK (said.find ("keyers: internal and external") != std::string::npos);
        CHECK (said.find ("formats it sends: 1080p50 1080p25 1080p59.94") != std::string::npos);
    }

    {
        Missing missing;
        std::ostringstream out;
        CHECK (video::runVideoProbe (missing, {}, out) == 2);
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
    output.kind = video::region::OutputKind::card;
    output.card = "dl-1a2b3c4d";
    output.videoMode = "2160p59.94";
    output.key = video::region::CardKey::internal;
    output.sound = { 0, 1, -1, 5 };

    video::region::InputReading input;
    input.id = "MC000071";
    input.kind = video::region::OutputKind::device;
    input.sender = "dl-1a2b3c4d";
    input.videoMode = "1080i50";

    video::region::writeConfig (*r, {}, { output }, { input });

    video::region::ConfigReading read;
    REQUIRE (video::region::readConfig (*r, read));
    REQUIRE (read.outputs.size() == 1);
    CHECK (read.outputs[0].kind == video::region::OutputKind::card);
    CHECK (read.outputs[0].toCard());
    CHECK_FALSE (read.outputs[0].sends());
    CHECK (read.outputs[0].card == "dl-1a2b3c4d");
    CHECK (read.outputs[0].videoMode == "2160p59.94");
    CHECK (read.outputs[0].key == video::region::CardKey::internal);
    CHECK (read.outputs[0].sound == std::vector<int> { 0, 1, -1, 5 });

    REQUIRE (read.inputs.size() == 1);
    CHECK (read.inputs[0].kind == video::region::OutputKind::device);
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
    CHECK (video::region::storedKind (99u) == video::region::OutputKind::device);
    CHECK (video::region::outputKindFrom ("card") == video::region::OutputKind::card);
    CHECK (video::region::outputKindFrom ("device") == video::region::OutputKind::device);
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
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/kind", "card").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/card", "fake-1").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/videoMode", "720p50").ok);

    //  A card whose clock runs a thousandth fast.
    setVariable ("WFG_CARD_FAKE", "1");
    setVariable ("WFG_CARD_FAKE_DRIFT", "1000");
    setVariable ("WFG_VIDEO_SOFTWARE", "1");

    Ticks ticks;
    video::VideoHost host { spec };
    host.configure (document);

    const auto running = tickUntil (host, ticks, [&host] { return host.readouts().renderer == "running"; });
    setVariable ("WFG_CARD_FAKE", nullptr);
    setVariable ("WFG_CARD_FAKE_DRIFT", nullptr);
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
    CHECK (output->skewPpm == doctest::Approx (1000.0).epsilon (0.15));
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

TEST_CASE ("decklink: the card's sound ring carries the lanes named, each slot stamped with its sample, and drops what it cannot hold (DL.6)")
{
    namespace cs = audio::cardsound;
    auto r = std::make_unique<cs::Region>();
    cs::stamp (*r);
    CHECK (cs::looksValid (*r));

    //  Four logical outputs, each a ramp of its own.
    constexpr int frames = 300;
    std::vector<std::vector<float>> outputs (4, std::vector<float> (frames));

    for (int c = 0; c < 4; ++c)
        for (int n = 0; n < frames; ++n)
            outputs[static_cast<std::size_t> (c)][static_cast<std::size_t> (n)] = static_cast<float> (c * 1000 + n);

    const float* pointers[4] { outputs[0].data(), outputs[1].data(), outputs[2].data(), outputs[3].data() };

    //  No lanes, nothing taken: a show with no card carrying sound costs a load.
    cs::write (*r, pointers, 4, frames, 48000, 48000);
    cs::Block block;
    CHECK_FALSE (cs::peek (*r, block));

    //  Outputs 3 and 0, and one the interface has not got - silence.
    cs::publishLanes (*r, { 3, 0, 9 });
    CHECK (cs::lanesOf (*r) == std::vector<int> { 3, 0, 9 });
    cs::write (*r, pointers, 4, frames, 48000, 48000);

    //  A block longer than a slot takes two, the second stamped where it starts.
    REQUIRE (cs::peek (*r, block));
    CHECK (block.sample == 48000);
    CHECK (block.frames == cs::slotFrames);
    CHECK (block.rate == 48000);
    CHECK (block.lanes == 3);
    CHECK (block.lane[0][10] == doctest::Approx (3010.0f));
    CHECK (block.lane[1][10] == doctest::Approx (10.0f));
    CHECK (block.lane[2][10] == doctest::Approx (0.0f));
    cs::release (*r);

    REQUIRE (cs::peek (*r, block));
    CHECK (block.sample == 48000 + cs::slotFrames);
    CHECK (block.frames == frames - cs::slotFrames);
    CHECK (block.lane[0][0] == doctest::Approx (static_cast<float> (3000 + cs::slotFrames)));
    cs::release (*r);
    CHECK_FALSE (cs::peek (*r, block));

    //  A reader that does not come: the ring fills, the rest is dropped and
    //  counted, and the next slot taken says how much went before it.
    std::int64_t at = 100000;

    for (int n = 0; n < cs::slotCount + 4; ++n, at += 128)
        cs::write (*r, pointers, 4, 128, at, 48000);

    CHECK (cs::droppedFrames (*r) == 4 * 128);

    for (int n = 0; n < cs::slotCount; ++n)
    {
        REQUIRE (cs::peek (*r, block));
        cs::release (*r);
    }

    cs::write (*r, pointers, 4, 128, at, 48000);
    REQUIRE (cs::peek (*r, block));
    CHECK (block.sample == at);
    CHECK (block.droppedBefore == 4 * 128);
}

TEST_CASE ("decklink: a card output's buses become the ring's lanes, each channel once, published when they change (DL.6)")
{
    namespace cs = audio::cardsound;

    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-lanes-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    cs::File file { folder.getFullPathName().toStdString() };
    REQUIRE (file.region() != nullptr);
    MESSAGE ("the ring's pages " << std::string (file.locked() ? "are" : "are not") << " locked into memory here");

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.headless = true;
    spec.cardSound = file.region();
    spec.cardSoundPath = file.path();

    doc::ShowDocument document;
    const juce::File bundle { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/video" };
    REQUIRE (doc::Bundle::open (bundle, document).ok);

    video::VideoHost host { spec };
    host.configure (document);
    CHECK (cs::lanesOf (*file.region()).empty());

    //  The fixture's stereo bus on the card: its two channels, once each.
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/kind", "card").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/card", "fake-1").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/sound", "VD000004 VD000004").ok);
    host.configure (document);
    CHECK (cs::lanesOf (*file.region()) == std::vector<int> { 0, 1 });

    //  Switched off, it carries nothing.
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/enabled", "false").ok);
    host.configure (document);
    CHECK (cs::lanesOf (*file.region()).empty());
}

namespace
{
    /*  Where a card's placed sound peaks on one channel, as a card sample. */
    std::int64_t peakOf (const std::vector<std::int32_t>& interleaved, int channels, int channel, std::int64_t first)
    {
        std::int64_t at = -1;
        std::int64_t best = 0;

        for (std::size_t n = static_cast<std::size_t> (channel); n < interleaved.size(); n += static_cast<std::size_t> (channels))
            if (std::abs (static_cast<std::int64_t> (interleaved[n])) > best)
            {
                best = std::abs (static_cast<std::int64_t> (interleaved[n]));
                at = first + static_cast<std::int64_t> (n / static_cast<std::size_t> (channels));
            }

        return at;
    }

    /*  Go.dot's sound in blocks of 480 at `rate` from `from` to `to`: a click
        on the first channel at `click`, a half on the second. */
    void feed (video::card::SoundPlacer& placer, int rate, std::int64_t from, std::int64_t to, std::int64_t click)
    {
        std::vector<float> first (480), second (480, 0.5f);

        for (auto at = from; at < to; at += 480)
        {
            std::fill (first.begin(), first.end(), 0.0f);

            if (click >= at && click < at + 480)
                first[static_cast<std::size_t> (click - at)] = 1.0f;

            placer.take (at, 480, rate, { first.data(), second.data() });
        }
    }
}

TEST_CASE ("decklink: a card's sound is placed by its sample - a click lands where the line says, at 48 and 44.1 kHz, through a drifting clock (DL.7)")
{
    for (const int rate : { 48000, 44100 })
    {
        CAPTURE (rate);
        video::card::SoundPlacer placer;
        placer.setChannels (2);

        //  Card sample k stands for Go.dot's 5000 + k at the source's rate,
        //  the card's clock a ten-thousandth slow against the interface's.
        const auto ratio = static_cast<double> (rate) / 48000.0 * 1.0001;
        const auto goDotAt = [ratio] (std::int64_t k) { return 5000.0 + static_cast<double> (k) * ratio; };

        feed (placer, rate, 4800, 4800 + 480 * 40, 10000);
        placer.startAt (0);

        std::vector<std::int32_t> all, out;

        while (placer.place (48000, goDotAt, out) > 0)
            all.insert (all.end(), out.begin(), out.end());

        //  What is not here yet is not placed: placing stops short of the history's end.
        REQUIRE (! all.empty());
        CHECK (goDotAt (placer.next()) < static_cast<double> (placer.knownUntil()));

        const auto expected = (10000.0 - 5000.0) / ratio;
        const auto peak = peakOf (all, 2, 0, 0);
        CHECK (std::abs (static_cast<double> (peak) - expected) <= 1.0);

        //  The second channel: the half, steady, wherever it was known.
        const auto middle = static_cast<std::size_t> (expected) * 2 + 1;
        CHECK (static_cast<double> (all[middle]) / 2147483647.0 == doctest::Approx (0.5).epsilon (0.001));
        CHECK (placer.late() == 0u);
    }

    //  READ WHERE NOTHING WAS KEPT: a card started before the first block's
    //  sample places silence, counted late, until it reaches what came.
    video::card::SoundPlacer placer;
    placer.setChannels (2);
    feed (placer, 48000, 20000, 30000, 25000);
    placer.startAt (0);
    std::vector<std::int32_t> out;
    placer.place (20000, [] (std::int64_t k) { return 10000.0 + static_cast<double> (k); }, out);
    CHECK (placer.late() >= 9990u);
    CHECK (peakOf (out, 2, 0, 0) == 15000);
}

TEST_CASE ("video gpu: a card output carrying a bus sends its sound behind the count with its picture, none late (§71, DL.7)")
{
    namespace cs = audio::cardsound;

    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-card-sound-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));
    cs::File file { folder.getFullPathName().toStdString() };
    REQUIRE (file.region() != nullptr);

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;
    spec.cardSound = file.region();
    spec.cardSoundPath = file.path();

    doc::ShowDocument document;
    const juce::File bundle { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/video" };
    REQUIRE (doc::Bundle::open (bundle, document).ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/kind", "card").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/card", "fake-1").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/videoMode", "720p50").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/sound", "VD000004").ok);

    setVariable ("WFG_CARD_FAKE", "1");
    setVariable ("WFG_VIDEO_SOFTWARE", "1");

    Ticks ticks;
    video::VideoHost host { spec };
    host.configure (document);
    CHECK (cs::lanesOf (*file.region()) == std::vector<int> { 0, 1 });

    /*  THE AUDIO THREAD, played by the test: two outputs, written in blocks
        of 480 as Go.dot's count goes by - a tone on the first. */
    std::int64_t written = ticks.now();
    std::vector<float> left (480), right (480, 0.0f);
    const float* pointers[2] { left.data(), right.data() };

    const auto pump = [&]
    {
        for (const auto now = ticks.now(); written + 480 <= now; written += 480)
        {
            for (int n = 0; n < 480; ++n)
                left[static_cast<std::size_t> (n)] = 0.25f * static_cast<float> (std::sin (static_cast<double> (written + n) * 0.06));

            cs::write (*file.region(), pointers, 2, 480, written, 48000);
        }
    };

    const auto running = tickUntil (host, ticks, [&] { pump(); return host.readouts().renderer == "running"; });
    setVariable ("WFG_CARD_FAKE", nullptr);
    setVariable ("WFG_VIDEO_SOFTWARE", nullptr);
    REQUIRE (running);

    REQUIRE (tickUntil (host, ticks, [&]
                        {
                            pump();
                            const auto said = host.readouts();
                            const auto* output = said.output ("VD000021");
                            return output != nullptr && output->bound && output->framesPresented > 10;
                        }, 15000));

    tickUntil (host, ticks, [&] { pump(); return false; }, 2000);

    const auto said = host.readouts();
    const auto* output = said.output ("VD000021");
    REQUIRE (output != nullptr);
    MESSAGE ("latency " << output->latencyMs << " ms, sound late or refused " << output->soundLate
             << ", frames late " << output->framesLate << ", ring dropped " << cs::droppedFrames (*file.region()));

    //  The picture and the sound 60 ms behind the count, with the frames waiting.
    CHECK (output->latencyMs >= 60.0);
    CHECK (output->latencyMs <= 100.5);
    CHECK (output->soundLate == 0u);
    CHECK (cs::droppedFrames (*file.region()) == 0);

    //  And the sound flows: some 48 000 frames a second, counted four times a second.
    const auto sentBefore = output->soundSent;
    const auto from = std::chrono::steady_clock::now();
    tickUntil (host, ticks, [&] { pump(); return false; }, 2000);
    const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - from).count();
    const auto latest = host.readouts();
    const auto* after = latest.output ("VD000021");
    REQUIRE (after != nullptr);
    const auto perSecond = static_cast<double> (after->soundSent - sentBefore) / seconds;
    MESSAGE ("sound frames a second: " << perSecond);
    CHECK (perSecond > 40000.0);
    CHECK (perSecond < 56000.0);
}

TEST_CASE ("decklink: an input's 8-bit YUV turned to colour - legal black and white, BT.709's red, BT.601 for a small picture (DL.8)")
{
    //  Four pixels: black, white, red as BT.709 codes it, and the same codes read as BT.601.
    const std::uint8_t row[] { 128, 16, 128, 16,     128, 235, 128, 235,     102, 63, 240, 63 };
    std::uint8_t bgra[6 * 4] {};

    video::card::uyvyToBgra (row, 6, 1, static_cast<int> (sizeof (row)), bgra, false);

    CHECK (bgra[0] == 0);
    CHECK (bgra[1] == 0);
    CHECK (bgra[2] == 0);
    CHECK (bgra[3] == 255);
    CHECK (bgra[8] == 255);
    CHECK (bgra[9] == 255);
    CHECK (bgra[10] == 255);

    //  Red: blue and green near nought, red near full.
    CHECK (bgra[16] <= 3);
    CHECK (bgra[17] <= 3);
    CHECK (bgra[18] >= 252);

    //  The same codes through BT.601: not the same red - which is why the
    //  height chooses.
    video::card::uyvyToBgra (row, 6, 1, static_cast<int> (sizeof (row)), bgra, true);
    CHECK (bgra[18] < 240);
    CHECK (bgra[18] > 220);
}

TEST_CASE ("video gpu: a card's input takes in what the test card's output sends - its size, format and rate - and says no signal when it stops (§71, DL.8)")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-card-in-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    const juce::File bundle { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/video" };
    REQUIRE (doc::Bundle::open (bundle, document).ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/kind", "card").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/card", "fake-1").ok);
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/videoMode", "720p50").ok);

    //  The test card's input, found by the card: it shows what its output last sent.
    REQUIRE (document.createVideoInput ("Loop", "device", "fake-1", "VD000031").ok);

    setVariable ("WFG_CARD_FAKE", "1");
    setVariable ("WFG_VIDEO_SOFTWARE", "1");

    Ticks ticks;
    video::VideoHost host { spec };
    host.configure (document);

    const auto running = tickUntil (host, ticks, [&host] { return host.readouts().renderer == "running"; });
    setVariable ("WFG_CARD_FAKE", nullptr);
    setVariable ("WFG_VIDEO_SOFTWARE", nullptr);
    REQUIRE (running);

    REQUIRE (tickUntil (host, ticks, [&host]
                        {
                            const auto said = host.readouts();
                            const auto* input = said.input ("VD000031");
                            //  Each frame turned to colour on the CPU: a Debug build keeps up with some.
                            return input != nullptr && input->connected && input->frameRate > 5.0;
                        }, 15000));

    {
        const auto said = host.readouts();
        const auto* input = said.input ("VD000031");
        REQUIRE (input != nullptr);
        MESSAGE ("taken in: " << input->width << "x" << input->height << " " << input->format << " at " << input->frameRate);
        CHECK (input->width == 1280);
        CHECK (input->height == 720);
        CHECK (input->format == "720p50");
        CHECK (input->problem.empty());
    }

    //  The output switched off: nothing comes, and the input says so (black, APM).
    REQUIRE (document.setAttribute ("/godot/videoOutput/VD000021/enabled", "false").ok);
    host.configure (document);

    CHECK (tickUntil (host, ticks, [&host]
                      {
                          const auto said = host.readouts();
                          const auto* input = said.input ("VD000031");
                          return input != nullptr && ! input->connected && input->problem == "no signal";
                      }, 10000));
}

TEST_CASE ("decklink: the probe runs the test card - bars, a flash and a beep each second - and says what came of them (DL.10)")
{
    {
        auto cards = video::card::makeFakeSystem();
        std::ostringstream out;
        CHECK (video::runVideoProbe (*cards, { "--run", "--seconds=1.2", "--sound", "--mode=720p50" }, out) == 0);

        const auto said = out.str();
        MESSAGE (said);
        CHECK (said.find ("Running Go.dot test card at 720p50, with sound, for 1.2 s") != std::string::npos);
        CHECK (said.find ("pixels: 8-bit BGRA, 1280 x 720, 50 frames a second") != std::string::npos);
        CHECK (said.find ("late: 0, dropped: 0") != std::string::npos);
        CHECK (said.find ("beeps placed: ") != std::string::npos);
        CHECK (said.find ("refused: 0") != std::string::npos);
        CHECK (said.find ("reference: none") != std::string::npos);

        //  Fifty a second, give or take the start: some sixty frames in 1.2 s.
        const auto at = said.find ("frames shown: ");
        REQUIRE (at != std::string::npos);
        CHECK (std::atoi (said.c_str() + at + 14) >= 40);
    }

    {
        auto cards = video::card::makeFakeSystem();
        juce::TemporaryFile report;
        std::ostringstream out;
        CHECK (video::runVideoProbe (*cards, { "--run", "--seconds=0.6", "--ten-bit", "--mode=720p50",
                                                  "--report=" + report.getFile().getFullPathName().toStdString() }, out) == 0);
        CHECK (out.str().find ("pixels: 10-bit YUV") != std::string::npos);
        CHECK (report.getFile().loadFileAsString().contains ("Go.dot video probe"));
    }

    //  No card that sends: said, and nothing run.
    video::card::FakeOptions absent;
    absent.present = false;
    auto none = video::card::makeFakeSystem (absent);
    std::ostringstream out;
    CHECK (video::runVideoProbe (*none, { "--run" }, out) == 2);
    CHECK (out.str().find ("No card to run: none sends pictures") != std::string::npos);
}

TEST_CASE ("camera: the test camera listed by name, its formats known once opened, automatic its largest at 25 a second or more, one input at a time (VC.1)")
{
    auto cameras = video::camera::oneInputEach (video::camera::makeFakeCameras());

    //  Listed by name, its formats not yet known: nothing was opened to list it.
    auto found = cameras->find();
    REQUIRE (found.cameras.size() == 2);
    CHECK (found.cameras[0].id == "cam:fake-1");
    CHECK (found.cameras[0].name == "Go.dot test camera");
    CHECK (found.cameras[0].formats.empty());
    CHECK (found.cameras[1].id == "cam:fake-2");
    CHECK (found.cameras[1].name == "Go.dot test camera, no picture");
    CHECK (video::camera::isCamera ("cam:fake-1"));
    CHECK_FALSE (video::camera::isCamera ("fake-1"));

    std::string why;
    auto source = cameras->open ("cam:fake-1", "auto", why);
    REQUIRE (source != nullptr);
    CHECK (source->formatWord() == "1080p30");
    CHECK (source->signal());

    //  Opened, its formats are known, and listed from then on.
    found = cameras->find();
    CHECK (found.cameras[0].formats == std::vector<std::string> { "720p30", "1080p30", "640x480p30" });

    //  A frame, then a newer one a thirtieth of a second on.
    std::vector<std::uint8_t> bgra;
    int w = 0, h = 0;
    std::uint64_t serial = 0;
    REQUIRE (source->newest (bgra, w, h, serial));
    CHECK (w == 1920);
    CHECK (h == 1080);
    CHECK (bgra.size() == 1920u * 1080u * 4u);
    const auto first = serial;
    waitMs (80);
    CHECK (source->newest (bgra, w, h, serial));
    CHECK (serial > first);

    //  One input at a time; a format it does not offer; a camera that is not there.
    std::string busy, notOffered, gone;
    CHECK (cameras->open ("cam:fake-1", "auto", busy) == nullptr);
    CHECK (busy == "the camera is already taken");
    source.reset();
    CHECK (cameras->open ("cam:fake-1", "1080p60", notOffered) == nullptr);
    CHECK (notOffered == "the camera does not offer 1080p60");
    CHECK (cameras->open ("cam:fake-9", "auto", gone) == nullptr);
    CHECK (gone == "the camera is not connected");

    //  One that opens and sends nothing: quiet, no signal, nothing wrong -
    //  and let go at once.
    std::string quietWhy;
    auto quiet = cameras->open ("cam:fake-2", "auto", quietWhy);
    REQUIRE (quiet != nullptr);
    waitMs (50);
    CHECK_FALSE (quiet->newest (bgra, w, h, serial));
    CHECK_FALSE (quiet->signal());
    CHECK (quiet->problem().empty());
    quiet.reset();

    //  Automatic: fast enough first, then the largest, then the faster.
    using video::camera::chooseAutomatic;
    CHECK (chooseAutomatic ({ "640x480p30", "1080p15", "720p30" }) == "720p30");
    CHECK (chooseAutomatic ({ "640x480p15", "1080p10" }) == "1080p10");
    CHECK (chooseAutomatic ({ "720p30", "720p60", "720p25" }) == "720p60");
    CHECK (chooseAutomatic ({ "1080p29.97", "1080p30" }) == "1080p30");
    CHECK (chooseAutomatic ({}).empty());
}

/*  THIS MACHINE'S OWN CAMERA (§72, VC.2-VC.4), on the bench only: opening a
    camera lights it, so it runs when WFG_CAMERA_REAL names one - a part of its
    name, or 1 for the first listed. Opens no window. */
TEST_CASE ("camera: a camera this machine lists, opened at automatic, its frames read, a second input refused, given back (VC.2)")
{
    const auto wanted = juce::SystemStats::getEnvironmentVariable ("WFG_CAMERA_REAL", {}).toStdString();

    if (wanted.empty())
        return;

    auto& cameras = video::camera::system();
    const auto found = cameras.find();
    CHECK (found.problem.empty());

    const video::camera::Camera* chosen = nullptr;

    for (const auto& one : found.cameras)
    {
        MESSAGE ("listed: " << one.name << "  " << one.id);

        if (chosen == nullptr && (wanted == "1" || one.name.find (wanted) != std::string::npos))
            chosen = &one;
    }

    REQUIRE (chosen != nullptr);
    const auto id = chosen->id;
    CHECK (chosen->formats.empty());

    std::string why;
    auto source = cameras.open (id, "auto", why);
    INFO (why);
    REQUIRE (source != nullptr);

    std::string offered;

    for (const auto& word : source->formats())
        offered += word + " ";

    MESSAGE ("opened at " << source->formatWord() << " of " << offered);
    const auto format = video::modes::parse (source->formatWord());
    REQUIRE (format.has_value());
    CHECK (source->formatWord() == video::camera::chooseAutomatic (source->formats()));

    //  Frames, at the size its format says, for two seconds.
    std::vector<std::uint8_t> bgra;
    int w = 0, h = 0, frames = 0;
    std::uint64_t serial = 0;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds (8);
    const auto firstAt = [&]
    {
        while (std::chrono::steady_clock::now() < until)
        {
            if (source->newest (bgra, w, h, serial))
                return true;

            waitMs (5);
        }

        return false;
    }();

    const auto problem = source->problem();
    INFO (problem);
    REQUIRE (firstAt);
    CHECK (w == format->width);
    CHECK (h == format->height);
    CHECK (bgra.size() == static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4);
    CHECK (source->signal());

    const auto from = std::chrono::steady_clock::now();

    while (std::chrono::steady_clock::now() - from < std::chrono::seconds (2))
    {
        if (source->newest (bgra, w, h, serial))
            ++frames;

        waitMs (2);
    }

    double sum = 0.0;

    for (std::size_t at = 0; at + 2 < bgra.size(); at += 4)
        sum += 0.0722 * bgra[at] + 0.7152 * bgra[at + 1] + 0.2126 * bgra[at + 2];

    MESSAGE ("frames a second: " << frames / 2.0 << ", mean light " << sum / (static_cast<double> (bgra.size()) / 4.0) << " of 255");
    CHECK (frames > 0);

    //  A second input on it refused; its formats now listed.
    std::string busy;
    CHECK (cameras.open (id, "auto", busy) == nullptr);
    CHECK (busy == "the camera is already taken");

    for (const auto& one : cameras.find().cameras)
        if (one.id == id)
            CHECK (one.formats == source->formats());

    //  Given back, it opens again.
    source.reset();
    std::string again;
    auto reopened = cameras.open (id, "auto", again);
    INFO (again);
    CHECK (reopened != nullptr);
}

TEST_CASE ("camera: YUYV turned to colour - legal and full range, BT.601 small and BT.709 large (VC.1)")
{
    //  Two pixels: legal black, legal white.
    const std::uint8_t legal[] { 16, 128, 235, 128 };
    std::uint8_t bgra[8] {};
    video::camera::yuyvToBgra (legal, 2, 1, 4, bgra, false);
    CHECK (bgra[0] == 0);
    CHECK (bgra[2] == 0);
    CHECK (bgra[4] == 255);
    CHECK (bgra[6] == 255);
    CHECK (bgra[7] == 255);

    //  The same codes read as full range: black is not black, white is not white.
    video::camera::yuyvToBgra (legal, 2, 1, 4, bgra, true);
    CHECK (bgra[0] > 10);
    CHECK (bgra[4] < 245);
}

TEST_CASE ("video gpu: a camera taken in by a renderer with no window, and what brings a picture in listed with it (§72, VC.1)")
{
    juce::TemporaryFile work;
    const auto folder = work.getFile().getSiblingFile ("godot-video-camera-" + juce::String (juce::Random::getSystemRandom().nextInt (1 << 30)));

    video::HostSpec spec;
    spec.workFolder = folder.getFullPathName().toStdString();
    spec.executable = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName().toStdString();
    spec.leadingArgs = { "video-render" };
    spec.headless = true;

    doc::ShowDocument document;
    const juce::File bundle { juce::String (std::string (WFG_TEST_FIXTURES_DIR)) + "/bundles/video" };
    REQUIRE (doc::Bundle::open (bundle, document).ok);
    REQUIRE (document.createVideoInput ("Lectern", "device", "cam:fake-1", "VD000032").ok);
    REQUIRE (document.createVideoInput ("Wings", "device", "cam:fake-9", "VD000033").ok);
    REQUIRE (document.createVideoInput ("Gallery", "device", "cam:fake-2", "VD000034").ok);

    setVariable ("WFG_CAMERA_FAKE", "1");
    setVariable ("WFG_CARD_FAKE", "1");
    setVariable ("WFG_VIDEO_SOFTWARE", "1");

    Ticks ticks;
    video::VideoHost host { spec };
    host.configure (document);

    const auto running = tickUntil (host, ticks, [&host] { return host.readouts().renderer == "running"; });
    setVariable ("WFG_CAMERA_FAKE", nullptr);
    setVariable ("WFG_CARD_FAKE", nullptr);
    setVariable ("WFG_VIDEO_SOFTWARE", nullptr);
    REQUIRE (running);

    REQUIRE (tickUntil (host, ticks, [&host]
                        {
                            const auto said = host.readouts();
                            const auto* input = said.input ("VD000032");
                            return input != nullptr && input->connected && input->frameRate > 5.0;
                        }, 15000));

    {
        const auto said = host.readouts();
        const auto* input = said.input ("VD000032");
        REQUIRE (input != nullptr);
        MESSAGE ("camera taken in: " << input->width << "x" << input->height << " " << input->format << " at " << input->frameRate);
        CHECK (input->width == 1920);
        CHECK (input->height == 1080);
        CHECK (input->format == "1080p30");
    }

    //  A camera that sends nothing: kept open, drawing nothing, no signal.
    CHECK (tickUntil (host, ticks, [&host]
                      {
                          const auto said = host.readouts();
                          const auto* input = said.input ("VD000034");
                          return input != nullptr && ! input->connected && input->problem == "no signal";
                      }, 5000));

    //  A camera that is not there: nothing drawn, and the input says why.
    CHECK (tickUntil (host, ticks, [&host]
                      {
                          const auto said = host.readouts();
                          const auto* input = said.input ("VD000033");
                          return input != nullptr && ! input->connected && input->problem == "the camera is not connected";
                      }, 5000));

    //  What brings a picture in: the test card's input, and the test camera
    //  with its formats now that it has been opened.
    CHECK (tickUntil (host, ticks, [&host]
                      {
                          const auto devices = host.readouts().devices;
                          return devices.find ("cam:fake-1\tGo.dot test camera\tcamera\t720p30 1080p30 640x480p30") != std::string::npos
                              && devices.find ("fake-1\tGo.dot test card\t\t1080p50") != std::string::npos;
                      }, 5000));
}
