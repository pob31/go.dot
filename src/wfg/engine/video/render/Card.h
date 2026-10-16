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

#pragma once

/*
    A VIDEO CARD, AS THE RENDERER SEES ONE (namespace draft §71, AOZ, APO).

    Everything in Go.dot that reaches a DeckLink card goes through this, and
    nothing here names the SDK: Card_decklink.cpp is the real card, through the
    Desktop Video the user installed; Card_fake.cpp is a TEST CARD that keeps
    time on the machine's clock, which every test uses and which
    WFG_DECKLINK_FAKE=1 puts in the real one's place, so the screens can be
    looked at with no card.

    AN OUTPUT is paced by the card (APB). The renderer fills a frame the card
    lent it and schedules it at a frame number on the card's stream; the card
    sends frame n at n frame-durations on its stream, on its own clock, and
    says afterwards whether each was sent in time. `clock` pairs the stream's
    time with the machine's steady clock, read together, which is how the
    renderer knows when frame n will leave the card and so which sample of the
    sound to draw it for.

    AN INPUT hands over its newest frame when asked (APM): 8-bit YUV, two
    pixels in four bytes (U Y V Y), the format the card found.

    THREADS. Any thread; each object is used by one at a time - an output's
    frames and sound by the renderer's threads, which take turns through its
    own lock. A thread that touches a card first holds a ThreadScope (COM on
    Windows, nothing elsewhere). Never the audio thread.
*/

#include <wfg/engine/video/VideoModes.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wfg::video::card
{
    /*  How a frame's pixels are laid out. */
    enum class PixelFormat
    {
        bgra8,   ///< 8 bits each of blue, green, red, alpha: a keyed output's (APG)
        yuv10,   ///< 10-bit 4:2:2, six pixels in sixteen bytes (v210): an output's (APD)
        yuv8,    ///< 8-bit 4:2:2, two pixels in four bytes (UYVY): an input's (APM)
    };

    enum class Keying { off, external, internal };

    enum class Reference { none, locked, unlocked };

    /*  A format a card offers, by its word (VideoModes.h) and as the card
        names it. */
    struct Mode
    {
        std::string word;
        std::string name;
        modes::Format format;
        bool output = false;   ///< the card can send it
        bool input = false;    ///< the card can take it in
    };

    /*  A card, as found. Its identifier is kept across restarts and does not
        move when another card is added; its name is what a person reads. */
    struct Card
    {
        std::string id;
        std::string name;
        bool output = false;
        bool input = false;
        bool internalKey = false;
        bool externalKey = false;
        bool tenBit = false;           ///< takes 10-bit YUV for output
        int audioChannels = 0;         ///< most it embeds; nought for none
        std::vector<Mode> modes;
    };

    /*  What a look for cards found: the cards, and why there are none when
        Desktop Video is missing or too old. */
    struct Found
    {
        std::vector<Card> cards;
        std::string version;    ///< Desktop Video's, as it says it
        std::string problem;
    };

    /*  How an output is opened. */
    struct OutputSettings
    {
        std::string mode = modes::defaultWord;
        Keying key = Keying::off;
        int audioChannels = 0;   ///< 0, 2, 8, 16 or 32
        bool tenBit = true;      ///< 10-bit YUV where the card takes it and nothing is keyed (APD)
    };

    /*  A frame lent to be filled, and returned by scheduling it or giving it
        back. */
    struct Frame
    {
        void* pixels = nullptr;
        int rowBytes = 0;
        int slot = -1;
    };

    /*  What became of scheduled frames, counted since the output opened. */
    struct Completions
    {
        std::int64_t shown = 0;
        std::int64_t late = 0;
        std::int64_t dropped = 0;
    };

    class Output
    {
    public:
        virtual ~Output() = default;

        virtual const Mode& mode() const = 0;
        virtual PixelFormat format() const = 0;

        /*  A frame to draw into; one with no pixels while every frame is
            with the card. */
        virtual Frame lend() = 0;

        /*  `frame`, filled, sent as frame `index` of the stream; false when
            the card refused it, and the frame is back either way. */
        virtual bool schedule (const Frame& frame, std::int64_t index) = 0;
        virtual void giveBack (const Frame& frame) = 0;

        /*  Frames scheduled and not yet sent. */
        virtual int waiting() = 0;

        /*  Sending starts with what was scheduled, frame `first` first. */
        virtual bool start (std::int64_t first) = 0;
        virtual bool started() const = 0;

        /*  The stream's time in nanoseconds - frame n leaves at n frame
            durations, between frames too - and the steady clock's nanoseconds,
            read together; false before it started. */
        virtual bool clock (std::int64_t& streamNanos, std::int64_t& steadyNanos) = 0;

        virtual Reference reference() = 0;
        virtual Completions completions() = 0;

        /*  SOUND WITH THE PICTURE (APK): interleaved 32-bit samples at 48 kHz,
            `channels` a frame, placed at `streamSample` on the stream's own
            48 kHz time; false when refused. Sample frames held, not yet sent. */
        virtual bool scheduleSound (const std::int32_t* samples, int frames, std::int64_t streamSample) = 0;
        virtual int soundWaiting() = 0;

        /*  What went wrong after it opened - the card unplugged - or empty. */
        virtual std::string problem() = 0;
    };

    class Input
    {
    public:
        virtual ~Input() = default;

        /*  The newest frame that arrived, copied, when it is newer than
            `serial`; `serial` moves to it. 8-bit YUV, `rowBytes` a row. */
        virtual bool newest (std::vector<std::uint8_t>& pixels, int& width, int& height, int& rowBytes, std::uint64_t& serial) = 0;

        /*  Whether a signal arrives, its format's word, and why not. */
        virtual bool signal() = 0;
        virtual std::string modeWord() = 0;
        virtual std::string problem() = 0;
    };

    class System
    {
    public:
        virtual ~System() = default;

        /*  The cards on this machine now: a moment's work, asked once a
            second. */
        virtual Found find() = 0;

        /*  The card `id`'s output, or input, opened; null and why when not -
            no such card, another program has it, the format is not offered. */
        virtual std::unique_ptr<Output> openOutput (const std::string& id, const OutputSettings& settings, std::string& why) = 0;
        virtual std::unique_ptr<Input> openInput (const std::string& id, const std::string& modeWord, std::string& why) = 0;
    };

    /*  The cards this process reaches: the test card when WFG_DECKLINK_FAKE is
        set to 1, the user's Desktop Video otherwise - made the first time,
        kept for the process. */
    System& system();

    /*  Desktop Video's own, behind system(). */
    std::unique_ptr<System> makeDeckLinkSystem();

    /*  THE TEST CARD (APO), for tests and for looking at the screens with no
        card: one card, "Go.dot test card", with an output, an input that shows
        what its output last sent, both keyers, 10-bit and 16 channels. Put in
        the real one's place by WFG_DECKLINK_FAKE=1, its clock drifted by
        WFG_DECKLINK_FAKE_DRIFT parts per million and every Nth frame late by
        WFG_DECKLINK_FAKE_DROP. */
    struct FakeOptions
    {
        double driftPpm = 0.0;     ///< its clock against the machine's
        int dropEvery = 0;         ///< every Nth frame sent late; nought for none
        bool present = true;       ///< false: no card found
    };

    std::unique_ptr<System> makeFakeSystem (const FakeOptions& options = {});

    /*  The test card's last frame sent and the sound it sent, for tests: the
        frame's pixels and layout, and every sample frame by stream sample. */
    struct FakeMonitor
    {
        std::vector<std::uint8_t> pixels;
        int width = 0;
        int height = 0;
        int rowBytes = 0;
        PixelFormat format = PixelFormat::bgra8;
        std::int64_t index = -1;
        std::vector<std::int32_t> sound;    ///< interleaved, from stream sample 0
        int channels = 0;
    };

    /*  What `output` - one of the test card's - has sent; empty for a real
        card's. */
    FakeMonitor monitorOf (Output& output);

    /*  COM's apartment for this thread while it lives, on Windows; nothing
        elsewhere. */
    class ThreadScope
    {
    public:
        ThreadScope();
        ~ThreadScope();
        ThreadScope (const ThreadScope&) = delete;
        ThreadScope& operator= (const ThreadScope&) = delete;

    private:
        bool entered = false;
    };

    /*  The words an output's settings are stored with (namespace draft §71.3),
        and back. */
    const char* keyingWord (Keying keying) noexcept;
    Keying keyingFrom (const std::string& word) noexcept;
    const char* referenceWord (Reference reference) noexcept;

    /*  Bytes a row of `width` pixels takes in `format`: v210's rows are padded
        to 128 bytes, as cards ask. */
    int rowBytesFor (PixelFormat format, int width) noexcept;

    /*  WHAT `videoOutput/cards` SAYS (namespace draft §71.3): a line a card -
        its identifier, its name, what it can do (output, input, internal,
        external, 10bit, soundN), the formats it sends and those it takes in,
        tab between, words within by spaces - or one line, problem and why. */
    std::string describe (const Found& found);

    /*  10-BIT YUV ON THE CPU (APD): what the renderer's v210 pass makes of
        RGBA floats, `width` by `height`, into rows of `rowBytes` - the
        reference it is held to, and the test card's way back to colours. */
    std::vector<std::uint8_t> packV210 (const float* rgba, int width, int height, int rowBytes);

    /*  Sample `i` of row `y` - Cb Y Cr Y ... in their order - of v210 rows. */
    int v210Sample (const std::vector<std::uint8_t>& packed, int rowBytes, int y, int i) noexcept;
}
