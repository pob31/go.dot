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
    A DECKLINK CARD'S INPUT (namespace draft §71, APM): a camera or a vision
    mixer in a cable, taken in as NDI's pictures are. A thread of its own opens
    the card's input - found by the card unless a format was said - tries again
    every two seconds while it cannot, and turns each new frame from the card's
    8-bit YUV into BGRA, BT.709 or BT.601 for a picture no taller than 576; the
    render thread uploads the newest once a frame, however many cues show it.

    NO SIGNAL DRAWS BLACK: the picture is let go when the card says nothing
    arrives, so a capture of it shows what lies beneath - black on an empty
    canvas - and the input says "no signal", where NDI's keeps its last frame.
*/

#include <wfg/engine/video/render/Card.h>
#include <wfg/engine/video/render/Receiver.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wfg::video::render
{
    namespace
    {
        double secondsNow() noexcept
        {
            return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        class DeckLinkReceiver final : public Receiver
        {
        public:
            DeckLinkReceiver (std::string cardToTake, std::string modeToTake)
                : cardId (std::move (cardToTake)), modeWord (std::move (modeToTake))
            {
                receiving = std::thread ([this] { receive(); });
            }

            ~DeckLinkReceiver() override
            {
                stopping.store (true);

                if (receiving.joinable())
                    receiving.join();

                release();
            }

            void update() override
            {
                //  No signal: the picture let go, so the capture draws black (APM).
                if (! arriving.load())
                {
                    release();
                    return;
                }

                std::vector<std::uint8_t> pixels;
                int w = 0, h = 0;

                {
                    const std::lock_guard<std::mutex> hold (lock);

                    if (frameNumber == uploaded)
                        return;

                    uploaded = frameNumber;
                    pixels.swap (newest);
                    w = newestWidth;
                    h = newestHeight;
                }

                if (w <= 0 || h <= 0 || pixels.size() < static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4)
                    return;

                const auto bgra = sg_query_pixelformat (SG_PIXELFORMAT_BGRA8).sample;

                if (! bgra)
                    for (std::size_t at = 0; at + 3 < pixels.size(); at += 4)
                        std::swap (pixels[at], pixels[at + 2]);

                release();

                sg_image_desc desc {};
                desc.width = w;
                desc.height = h;
                desc.pixel_format = bgra ? SG_PIXELFORMAT_BGRA8 : SG_PIXELFORMAT_RGBA8;
                desc.data.mip_levels[0].ptr = pixels.data();
                desc.data.mip_levels[0].size = static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4;
                image = sg_make_image (desc);

                sg_view_desc view {};
                view.texture.image = image;
                textureView = sg_make_view (view);
                pictureWidth = w;
                pictureHeight = h;

                const std::lock_guard<std::mutex> hold (lock);

                if (spare.capacity() < pixels.capacity())
                    spare.swap (pixels);
            }

            sg_view picture() const override     { return textureView; }
            int width() const override           { return pictureWidth; }
            int height() const override          { return pictureHeight; }
            bool connected() const override      { return arriving.load() && textureView.id != SG_INVALID_ID; }

            double frameRate() const override
            {
                const std::lock_guard<std::mutex> hold (lock);
                return arrivals.rate (secondsNow());
            }

            std::string problem() const override
            {
                const std::lock_guard<std::mutex> hold (lock);
                return trouble;
            }

            double age() const override
            {
                const std::lock_guard<std::mutex> hold (lock);
                return arrivals.age (secondsNow());
            }

            std::string format() const override
            {
                const std::lock_guard<std::mutex> hold (lock);
                return arrivingFormat;
            }

        private:
            void say (const std::string& text)
            {
                const std::lock_guard<std::mutex> hold (lock);
                trouble = text;
            }

            /*  THE RECEIVING THREAD: the card's input opened, tried again
                every two seconds while it cannot be, then its newest frame
                taken every few milliseconds and turned to BGRA. */
            void receive()
            {
                const card::ThreadScope apartment;
                std::unique_ptr<card::Input> input;
                double retryAt = 0.0;
                std::uint64_t serial = 0;
                std::vector<std::uint8_t> yuv;

                while (! stopping.load())
                {
                    if (input == nullptr)
                    {
                        if (secondsNow() < retryAt)
                        {
                            std::this_thread::sleep_for (std::chrono::milliseconds (50));
                            continue;
                        }

                        std::string why;
                        input = card::system().openInput (cardId, modeWord, why);
                        retryAt = secondsNow() + 2.0;

                        if (input == nullptr)
                        {
                            say (why.empty() ? std::string ("the card's input would not open") : why);
                            continue;
                        }
                    }

                    int w = 0, h = 0, rowBytes = 0;

                    if (input->newest (yuv, w, h, rowBytes, serial))
                        keep (yuv, w, h, rowBytes, input->modeWord());

                    const auto signal = input->signal();
                    arriving.store (signal);

                    if (! signal)
                        say (input->problem().empty() ? std::string ("no signal") : input->problem());
                    else
                        say ({});

                    {
                        const std::lock_guard<std::mutex> hold (lock);
                        arrivingFormat = signal ? input->modeWord() : std::string();
                    }

                    std::this_thread::sleep_for (std::chrono::milliseconds (4));
                }
            }

            void keep (const std::vector<std::uint8_t>& yuv, int w, int h, int rowBytes, const std::string&)
            {
                std::vector<std::uint8_t> pixels;

                {
                    const std::lock_guard<std::mutex> hold (lock);
                    pixels.swap (spare);
                }

                pixels.resize (static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4);
                card::uyvyToBgra (yuv.data(), w, h, rowBytes, pixels.data(), h <= 576);

                const std::lock_guard<std::mutex> hold (lock);
                newest.swap (pixels);
                newestWidth = w;
                newestHeight = h;
                ++frameNumber;
                arrivals.arrived (secondsNow());
            }

            void release()
            {
                if (textureView.id != SG_INVALID_ID)
                    sg_destroy_view (textureView);

                if (image.id != SG_INVALID_ID)
                    sg_destroy_image (image);

                textureView = {};
                image = {};
            }

            const std::string cardId;
            const std::string modeWord;
            std::thread receiving;
            std::atomic<bool> stopping { false };
            std::atomic<bool> arriving { false };

            mutable std::mutex lock;
            std::vector<std::uint8_t> newest, spare;
            int newestWidth = 0;
            int newestHeight = 0;
            std::uint64_t frameNumber = 0;
            std::uint64_t uploaded = 0;
            ArrivalRate arrivals;
            std::string trouble;
            std::string arrivingFormat;

            sg_image image {};
            sg_view textureView {};
            int pictureWidth = 0;
            int pictureHeight = 0;
        };
    }

    std::unique_ptr<Receiver> makeDeckLinkReceiver (const std::string& cardId, const std::string& mode, std::string& why)
    {
        if (cardId.empty())
        {
            why = "no card chosen";
            return nullptr;
        }

        return std::make_unique<DeckLinkReceiver> (cardId, mode.empty() ? std::string (modes::automatic) : mode);
    }
}
