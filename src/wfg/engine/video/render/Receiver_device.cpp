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
    A VIDEO CAPTURE INPUT (namespace draft §71 APM, §72 APR): a picture a
    device on this machine brings in - a DeckLink or AJA card's input, or a
    camera - taken in as NDI's pictures are. Which it is, its identifier says:
    `cam:` a camera (Camera.h), anything else a card's input (Card.h).

    A thread of its own opens it - at the format asked for, or the one the card
    finds or `auto` picks for a camera - and tries again every two seconds while
    it cannot; takes the newest frame every few milliseconds, a card's turned
    from 8-bit YUV to BGRA (BT.709, BT.601 no taller than 576), a camera's BGRA
    already; and the render thread uploads the newest once a frame
    (FrameUpload.h).

    NO SIGNAL DRAWS BLACK: the picture is let go when nothing arrives, so a
    capture of it shows what lies beneath, and the input says why. A card with
    no cable in stays open, waiting, as does a camera that sends nothing; a
    camera that stopped - unplugged, taken by another program - is let go and
    opened again when it can be.
*/

#include <wfg/engine/video/render/Camera.h>
#include <wfg/engine/video/render/Card.h>
#include <wfg/engine/video/render/FrameUpload.h>
#include <wfg/engine/video/render/Receiver.h>

#include <wfg/engine/video/DeviceNames.h>
#include <wfg/engine/video/VideoModes.h>

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

        class DeviceReceiver final : public Receiver
        {
        public:
            DeviceReceiver (std::string deviceToTake, std::string modeToTake)
                : deviceId (std::move (deviceToTake)), modeWord (std::move (modeToTake)), camera (camera::isCamera (deviceId))
            {
                receiving = std::thread ([this] { receive(); });
            }

            ~DeviceReceiver() override
            {
                stopping.store (true);

                if (receiving.joinable())
                    receiving.join();
            }

            void update() override
            {
                //  No signal: the picture let go, so the capture draws black.
                if (! arriving.load())
                {
                    newest.release();
                    return;
                }

                newest.upload();
            }

            sg_view picture() const override     { return newest.view(); }
            int width() const override           { return newest.width(); }
            int height() const override          { return newest.height(); }
            bool connected() const override      { return arriving.load() && newest.view().id != SG_INVALID_ID; }

            double frameRate() const override
            {
                const std::scoped_lock hold (lock);
                return arrivals.rate (secondsNow());
            }

            std::string problem() const override
            {
                const std::scoped_lock hold (lock);
                return trouble;
            }

            double age() const override
            {
                const std::scoped_lock hold (lock);
                return arrivals.age (secondsNow());
            }

            std::string format() const override
            {
                const std::scoped_lock hold (lock);
                return arrivingFormat;
            }

        private:
            void say (const std::string& text, const std::string& formatNow)
            {
                const std::scoped_lock hold (lock);
                trouble = text;
                arrivingFormat = formatNow;
            }

            void arrived()
            {
                const std::scoped_lock hold (lock);
                arrivals.arrived (secondsNow());
            }

            /*  THE RECEIVING THREAD: the device opened, tried again every two
                seconds while it cannot be, then its newest frame taken every
                few milliseconds. */
            void receive()
            {
                const card::ThreadScope apartment;
                std::unique_ptr<card::Input> cardInput;
                std::unique_ptr<camera::Source> cameraSource;
                double retryAt = 0.0;
                std::uint64_t serial = 0;
                std::vector<std::uint8_t> yuv;

                while (! stopping.load())
                {
                    if (cardInput == nullptr && cameraSource == nullptr)
                    {
                        if (secondsNow() < retryAt)
                        {
                            std::this_thread::sleep_for (std::chrono::milliseconds (50));
                            continue;
                        }

                        std::string why;
                        retryAt = secondsNow() + 2.0;
                        serial = 0;

                        if (camera)
                            cameraSource = camera::system().open (deviceId, modeWord, why);
                        else
                            cardInput = card::system().openInput (deviceId, modeWord, why);

                        if (cardInput == nullptr && cameraSource == nullptr)
                        {
                            arriving.store (false);
                            say (why.empty() ? std::string ("the device would not open") : why, {});
                            continue;
                        }
                    }

                    if (cameraSource != nullptr)
                    {
                        auto bgra = newest.spare();
                        int w = 0, h = 0;

                        if (cameraSource->newest (bgra, w, h, serial))
                        {
                            newest.keep (std::move (bgra), w, h);
                            arrived();
                        }

                        const auto signal = cameraSource->signal();
                        const auto stopped = cameraSource->problem();
                        arriving.store (signal);
                        say (signal ? std::string() : (stopped.empty() ? std::string ("no signal") : stopped),
                             signal ? cameraSource->formatWord() : std::string());

                        //  A camera that stopped is let go, and opened again in
                        //  two seconds; a quiet one is kept, waiting.
                        if (! signal && ! stopped.empty())
                        {
                            cameraSource.reset();
                            retryAt = secondsNow() + 2.0;
                        }
                    }
                    else
                    {
                        int w = 0, h = 0, rowBytes = 0;

                        if (cardInput->newest (yuv, w, h, rowBytes, serial))
                        {
                            auto bgra = newest.spare();
                            bgra.resize (static_cast<std::size_t> (w) * static_cast<std::size_t> (h) * 4);
                            card::uyvyToBgra (yuv.data(), w, h, rowBytes, bgra.data(), h <= 576);
                            newest.keep (std::move (bgra), w, h);
                            arrived();
                        }

                        const auto signal = cardInput->signal();
                        arriving.store (signal);
                        say (signal ? std::string() : (cardInput->problem().empty() ? std::string ("no signal") : cardInput->problem()),
                             signal ? cardInput->modeWord() : std::string());
                    }

                    std::this_thread::sleep_for (std::chrono::milliseconds (4));
                }
            }

            const std::string deviceId;
            const std::string modeWord;
            const bool camera;
            std::thread receiving;
            std::atomic<bool> stopping { false };
            std::atomic<bool> arriving { false };

            NewestPicture newest;

            mutable std::mutex lock;
            ArrivalRate arrivals;
            std::string trouble;
            std::string arrivingFormat;
        };
    }

    std::unique_ptr<Receiver> makeDeviceReceiver (const std::string& deviceId, const std::string& mode, std::string& why)
    {
        if (deviceId.empty())
        {
            why = "no device chosen";
            return nullptr;
        }

        return std::make_unique<DeviceReceiver> (deviceId, mode.empty() ? std::string (modes::automatic) : mode);
    }

    std::string discoverDevices (const card::Found& cards)
    {
        /*  WHAT BRINGS A PICTURE IN (`videoInput/devices`): each card that
            takes pictures in and each camera, a line each - its identifier,
            its name, its maker, the formats it takes in - or a camera's, once
            it has been opened. */
        std::string out;

        const auto line = [&out] (const std::string& id, const std::string& name, const std::string& maker,
                                  const std::vector<std::string>& formats)
        {
            std::string words;

            for (const auto& word : formats)
                words += (words.empty() ? "" : " ") + word;

            out += (out.empty() ? "" : "\n") + id + "\t" + name + "\t" + maker + "\t" + words;
        };

        for (const auto& card : cards.cards)
        {
            if (! card.input)
                continue;

            std::vector<std::string> takes;

            for (const auto& mode : card.modes)
                if (mode.input)
                    takes.push_back (mode.word);

            line (card.id, card.name, makerOf (card.id), takes);
        }

        for (const auto& found : camera::system().find().cameras)
            line (found.id, found.name, "camera", found.formats);

        return out;
    }
}
