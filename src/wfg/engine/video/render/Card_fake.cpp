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
    THE TEST CARD (namespace draft §71, APO). One card that keeps time on the
    machine's steady clock - drifted by as many parts per million as a test
    asks - and does what a DeckLink card does with what it is given: frames
    lent, filled, scheduled at a frame number and sent when the stream reaches
    it, each counted shown, late (scheduled after its moment) or dropped; sound
    placed on the stream's 48 kHz; an input showing what the output last sent.

    It runs on no thread of its own: every call first brings the card up to
    now, so what a test sees depends only on the clock and the calls made.
*/

#include <wfg/engine/video/render/Card.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>

namespace wfg::video::card
{
    namespace
    {
        constexpr const char* cardId = "fake-1";
        constexpr const char* cardName = "Go.dot test card";
        constexpr int framesInPool = 8;
        constexpr int soundChannels = 16;
        constexpr std::int64_t soundRate = 48000;
        constexpr std::int64_t soundHeld = soundRate * 120;   // two minutes, for a test's monitor

        std::int64_t steadyNanos()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        std::vector<Mode> fakeModes()
        {
            std::vector<Mode> offered;

            for (const char* word : { "1080p50", "1080p25", "1080p59.94", "1080p60", "1080i50", "1080i59.94",
                                      "720p50", "2160p25", "2160p50", "2160p59.94", "dci1080p24" })
                if (const auto format = modes::parse (word))
                    offered.push_back ({ word, word, *format, true, true });

            return offered;
        }

        /*  BT.709 at legal levels, 8 bits: what a card would make of the
            colours it was sent. */
        void toYuv (int r, int g, int b, int& y, int& u, int& v)
        {
            const double yf = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            y = static_cast<int> (std::lround (16.0 + yf * 219.0 / 255.0));
            u = static_cast<int> (std::lround (128.0 + (b - yf) / 1.8556 * 224.0 / 255.0));
            v = static_cast<int> (std::lround (128.0 + (r - yf) / 1.5748 * 224.0 / 255.0));
            y = std::clamp (y, 0, 255);
            u = std::clamp (u, 0, 255);
            v = std::clamp (v, 0, 255);
        }

        /*  A sent frame as an input hands it over: 8-bit 4:2:2, U Y V Y. */
        std::vector<std::uint8_t> toUyvy (const FakeMonitor& sent)
        {
            const int width = sent.width & ~1;
            std::vector<std::uint8_t> out (static_cast<std::size_t> (width * 2 * sent.height));

            for (int row = 0; row < sent.height; ++row)
            {
                const auto* in = sent.pixels.data() + static_cast<std::size_t> (row * sent.rowBytes);
                auto* to = out.data() + static_cast<std::size_t> (row * width * 2);

                if (sent.format == PixelFormat::bgra8)
                {
                    for (int x = 0; x < width; x += 2)
                    {
                        int y0 = 0, u0 = 0, v0 = 0, y1 = 0, u1 = 0, v1 = 0;
                        toYuv (in[x * 4 + 2], in[x * 4 + 1], in[x * 4], y0, u0, v0);
                        toYuv (in[x * 4 + 6], in[x * 4 + 5], in[x * 4 + 4], y1, u1, v1);
                        to[x * 2]     = static_cast<std::uint8_t> ((u0 + u1 + 1) / 2);
                        to[x * 2 + 1] = static_cast<std::uint8_t> (y0);
                        to[x * 2 + 2] = static_cast<std::uint8_t> ((v0 + v1 + 1) / 2);
                        to[x * 2 + 3] = static_cast<std::uint8_t> (y1);
                    }
                }
                else if (sent.format == PixelFormat::yuv10)
                {
                    //  v210: each 32-bit word three 10-bit samples, low bits
                    //  first, in the order Cb Y Cr Y Cb Y Cr Y ... - the order
                    //  UYVY already has, so each sample is its top eight bits.
                    std::size_t sample = 0;
                    const std::size_t samples = static_cast<std::size_t> (width) * 2;

                    for (std::size_t word = 0; sample < samples; ++word)
                    {
                        std::uint32_t packed = 0;
                        std::memcpy (&packed, in + word * 4, 4);

                        for (int third = 0; third < 3 && sample < samples; ++third, ++sample)
                            to[sample] = static_cast<std::uint8_t> (((packed >> (10 * third)) & 0x3ffu) >> 2);
                    }
                }
                else
                {
                    std::memcpy (to, in, static_cast<std::size_t> (width * 2));
                }
            }

            return out;
        }

        /*  What the card's output and input share: the last frame sent. */
        struct Loop
        {
            std::mutex lock;
            FakeMonitor sent;
            std::uint64_t serial = 0;
            std::int64_t sentAt = 0;   ///< steady nanoseconds
            std::string modeWord;
            bool outputOpen = false;
            bool inputOpen = false;
        };

        class FakeOutput final : public Output
        {
        public:
            FakeOutput (Mode modeToUse, const OutputSettings& settings, const FakeOptions& optionsToUse, std::shared_ptr<Loop> loopToUse)
                : chosen (std::move (modeToUse)),
                  pixelFormat (settings.key == Keying::off ? PixelFormat::yuv10 : PixelFormat::bgra8),
                  options (optionsToUse),
                  loop (std::move (loopToUse)),
                  channels (settings.audioChannels)
            {
                rowBytes = rowBytesFor (pixelFormat, chosen.format.width);

                for (auto& slot : slots)
                    slot.pixels.assign (static_cast<std::size_t> (rowBytes * chosen.format.height), 0);

                monitor.width = chosen.format.width;
                monitor.height = chosen.format.height;
                monitor.rowBytes = rowBytes;
                monitor.format = pixelFormat;
                monitor.channels = channels;
            }

            ~FakeOutput() override
            {
                const std::scoped_lock hold (loop->lock);
                loop->outputOpen = false;
            }

            const Mode& mode() const override          { return chosen; }
            PixelFormat format() const override        { return pixelFormat; }

            Frame lend() override
            {
                const std::scoped_lock hold (lock);
                advance();

                for (int i = 0; i < framesInPool; ++i)
                    if (slots[static_cast<std::size_t> (i)].state == State::free)
                    {
                        auto& slot = slots[static_cast<std::size_t> (i)];
                        slot.state = State::lent;
                        return { slot.pixels.data(), rowBytes, i };
                    }

                return {};
            }

            bool schedule (const Frame& frame, std::int64_t index) override
            {
                const std::scoped_lock hold (lock);
                advance();

                if (frame.slot < 0 || frame.slot >= framesInPool || scheduled.count (index) != 0)
                {
                    giveBackLocked (frame);
                    return false;
                }

                auto& slot = slots[static_cast<std::size_t> (frame.slot)];
                const auto now = running ? streamTimeNow() : 0;

                //  Scheduled once its moment had gone: never sent, as a card
                //  drops it. Scheduled during its moment: sent late.
                if (running && (index + 1) * chosen.format.frameDuration <= now)
                {
                    ++counted.dropped;
                    slot.state = State::free;
                    return true;
                }

                slot.state = State::scheduled;
                slot.late = running && index * chosen.format.frameDuration < now;
                scheduled[index] = frame.slot;
                return true;
            }

            void giveBack (const Frame& frame) override
            {
                const std::scoped_lock hold (lock);
                giveBackLocked (frame);
            }

            int waiting() override
            {
                const std::scoped_lock hold (lock);
                advance();
                return static_cast<int> (scheduled.size());
            }

            bool start (std::int64_t first) override
            {
                const std::scoped_lock hold (lock);

                if (running)
                    return false;

                running = true;
                startNanos = steadyNanos();
                startTime = first * chosen.format.frameDuration;
                return true;
            }

            bool started() const override
            {
                const std::scoped_lock hold (lock);
                return running;
            }

            bool clock (std::int64_t& streamNanos, std::int64_t& nanos) override
            {
                const std::scoped_lock hold (lock);

                if (! running)
                    return false;

                nanos = steadyNanos();
                streamNanos = streamNanosAt (nanos);
                return true;
            }

            Reference reference() override      { return Reference::none; }

            Completions completions() override
            {
                const std::scoped_lock hold (lock);
                advance();
                return counted;
            }

            bool scheduleSound (const std::int32_t* samples, int frames, std::int64_t streamSample) override
            {
                const std::scoped_lock hold (lock);

                if (channels <= 0 || frames <= 0 || streamSample < 0 || streamSample + frames > soundHeld)
                    return false;

                const auto end = static_cast<std::size_t> ((streamSample + frames) * channels);

                if (monitor.sound.size() < end)
                    monitor.sound.resize (end, 0);

                std::memcpy (monitor.sound.data() + streamSample * channels, samples,
                             static_cast<std::size_t> (frames * channels) * sizeof (std::int32_t));
                soundEnd = std::max (soundEnd, streamSample + frames);
                return true;
            }

            int soundWaiting() override
            {
                const std::scoped_lock hold (lock);

                if (! running)
                    return static_cast<int> (soundEnd);

                const auto now = streamNanosAt (steadyNanos()) * soundRate / 1'000'000'000;
                return static_cast<int> (std::max<std::int64_t> (0, soundEnd - now));
            }

            std::string problem() override      { return {}; }

            FakeMonitor copyOfMonitor() const
            {
                const std::scoped_lock hold (lock);
                return monitor;
            }

        private:
            enum class State { free, lent, scheduled };

            struct Slot
            {
                std::vector<std::uint8_t> pixels;
                State state = State::free;
                bool late = false;
            };

            void giveBackLocked (const Frame& frame)
            {
                if (frame.slot >= 0 && frame.slot < framesInPool && slots[static_cast<std::size_t> (frame.slot)].state == State::lent)
                    slots[static_cast<std::size_t> (frame.slot)].state = State::free;
            }

            /*  The stream's nanoseconds at the machine's `nanos`: from where it
                started, at the card's rate. */
            std::int64_t streamNanosAt (std::int64_t nanos) const
            {
                const double elapsed = static_cast<double> (nanos - startNanos) * (1.0 + options.driftPpm * 1.0e-6);
                return startTime * 1'000'000'000 / chosen.format.timeScale + static_cast<std::int64_t> (std::floor (elapsed));
            }

            /*  The same in the mode's time scale. */
            std::int64_t streamTimeAt (std::int64_t nanos) const
            {
                return streamNanosAt (nanos) * chosen.format.timeScale / 1'000'000'000;
            }

            std::int64_t streamTimeNow() const      { return streamTimeAt (steadyNanos()); }

            /*  Every scheduled frame whose time on the stream has passed has
                been sent: counted, kept as the last frame, its slot free. */
            void advance()
            {
                if (! running)
                    return;

                const auto now = streamTimeNow();

                while (! scheduled.empty() && (scheduled.begin()->first + 1) * chosen.format.frameDuration <= now)
                {
                    const auto [index, slotIndex] = *scheduled.begin();
                    scheduled.erase (scheduled.begin());
                    auto& slot = slots[static_cast<std::size_t> (slotIndex)];

                    if (slot.late || (options.dropEvery > 0 && index % options.dropEvery == 0))
                        ++counted.late;
                    else
                        ++counted.shown;

                    monitor.pixels = slot.pixels;
                    monitor.index = index;
                    slot.state = State::free;
                    slot.late = false;

                    const std::scoped_lock shared (loop->lock);
                    loop->sent = monitor;
                    loop->sent.sound.clear();
                    loop->modeWord = chosen.word;
                    loop->sentAt = steadyNanos();
                    ++loop->serial;
                }
            }

            const Mode chosen;
            const PixelFormat pixelFormat;
            const FakeOptions options;
            const std::shared_ptr<Loop> loop;
            const int channels;
            int rowBytes = 0;

            mutable std::mutex lock;
            std::array<Slot, framesInPool> slots;
            std::map<std::int64_t, int> scheduled;
            bool running = false;
            std::int64_t startNanos = 0;
            std::int64_t startTime = 0;
            std::int64_t soundEnd = 0;
            Completions counted;
            FakeMonitor monitor;
        };

        class FakeInput final : public Input
        {
        public:
            FakeInput (std::string modeToUse, std::shared_ptr<Loop> loopToUse)
                : wanted (std::move (modeToUse)), loop (std::move (loopToUse)) {}

            ~FakeInput() override
            {
                const std::scoped_lock hold (loop->lock);
                loop->inputOpen = false;
            }

            bool newest (std::vector<std::uint8_t>& pixels, int& width, int& height, int& rowBytes, std::uint64_t& serial) override
            {
                const std::scoped_lock hold (loop->lock);

                if (! arriving() || loop->serial == serial)
                    return false;

                pixels = toUyvy (loop->sent);
                width = loop->sent.width & ~1;
                height = loop->sent.height;
                rowBytes = width * 2;
                serial = loop->serial;
                return true;
            }

            bool signal() override
            {
                const std::scoped_lock hold (loop->lock);
                return arriving();
            }

            std::string modeWord() override
            {
                const std::scoped_lock hold (loop->lock);
                return arriving() ? loop->modeWord : std::string();
            }

            std::string problem() override
            {
                const std::scoped_lock hold (loop->lock);
                return arriving() ? std::string() : std::string ("no signal");
            }

        private:
            /*  A picture within the last half second, in the format asked for
                - any, when found by the card. */
            bool arriving() const
            {
                return loop->serial != 0 && steadyNanos() - loop->sentAt < 500'000'000
                    && (wanted == modes::automatic || wanted == loop->modeWord);
            }

            const std::string wanted;
            const std::shared_ptr<Loop> loop;
        };

        class FakeSystem final : public System
        {
        public:
            explicit FakeSystem (const FakeOptions& optionsToUse) : options (optionsToUse) {}

            Found find() override
            {
                Found found;
                found.version = "test card";

                if (! options.present)
                    return found;

                Card card;
                card.id = cardId;
                card.name = cardName;
                card.output = card.input = card.internalKey = card.externalKey = card.tenBit = true;
                card.audioChannels = soundChannels;
                card.modes = fakeModes();
                found.cards.push_back (std::move (card));
                return found;
            }

            std::unique_ptr<Output> openOutput (const std::string& id, const OutputSettings& settings, std::string& why) override
            {
                const auto mode = modeFor (id, settings.mode, why);

                if (! mode)
                    return nullptr;

                if (settings.audioChannels > soundChannels)
                {
                    why = "the card embeds at most 16 channels";
                    return nullptr;
                }

                const std::scoped_lock hold (loop->lock);

                if (loop->outputOpen)
                {
                    why = "another program has the card";
                    return nullptr;
                }

                loop->outputOpen = true;
                return std::make_unique<FakeOutput> (*mode, settings, options, loop);
            }

            std::unique_ptr<Input> openInput (const std::string& id, const std::string& modeWord, std::string& why) override
            {
                if (modeWord != modes::automatic && ! modeFor (id, modeWord, why))
                    return nullptr;

                if (! options.present || id != cardId)
                {
                    why = "the card is not connected";
                    return nullptr;
                }

                const std::scoped_lock hold (loop->lock);

                if (loop->inputOpen)
                {
                    why = "the card's input is already taken";
                    return nullptr;
                }

                loop->inputOpen = true;
                return std::make_unique<FakeInput> (modeWord, loop);
            }

        private:
            std::optional<Mode> modeFor (const std::string& id, const std::string& word, std::string& why) const
            {
                if (! options.present || id != cardId)
                {
                    why = "the card is not connected";
                    return std::nullopt;
                }

                for (const auto& mode : fakeModes())
                    if (mode.word == word)
                        return mode;

                why = "the card cannot send " + word;
                return std::nullopt;
            }

            const FakeOptions options;
            const std::shared_ptr<Loop> loop = std::make_shared<Loop>();
        };
    }

    std::unique_ptr<System> makeFakeSystem (const FakeOptions& options)
    {
        return std::make_unique<FakeSystem> (options);
    }

    FakeMonitor monitorOf (Output& output)
    {
        if (auto* fake = dynamic_cast<FakeOutput*> (&output))
            return fake->copyOfMonitor();

        return {};
    }
}
