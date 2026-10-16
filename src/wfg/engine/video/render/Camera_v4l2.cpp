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
    CAMERAS ON LINUX, THROUGH VIDEO4LINUX (namespace draft §72, APS).

    Every /dev/video* that captures pictures and streams them - a camera's
    metadata node does neither and is passed over - listed by the name the
    driver gives and identified by its /dev/v4l/by-id link where there is one,
    which follows the camera from port to port; asking its name lights nothing.
    Opened, its formats are read - each size and rate of its YUYV and MJPEG, an
    uncompressed one before MJPEG at a size - the one asked for set, and frames
    streamed through four mapped buffers on a thread of its own: YUYV turned to
    colour at the levels the driver says, MJPEG decoded by JUCE.

    Built by CI and the NUC; no camera on either. No dependency: the V4L2
    header comes with the kernel's.
*/

#include <wfg/engine/video/render/Camera.h>

#include <wfg/engine/video/VideoModes.h>

#include <juce_graphics/juce_graphics.h>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>

namespace wfg::video::camera
{
    namespace
    {
        int control (int fd, unsigned long request, void* argument)
        {
            int result = -1;

            do
                result = ioctl (fd, request, argument);
            while (result == -1 && errno == EINTR);

            return result;
        }

        /*  /dev/videoN by the /dev/v4l/by-id link that names it, where one does. */
        std::map<std::string, std::string> stableNames()
        {
            std::map<std::string, std::string> byDevice;
            std::error_code error;

            for (const auto& entry : std::filesystem::directory_iterator ("/dev/v4l/by-id", error))
            {
                const auto target = std::filesystem::canonical (entry.path(), error);

                if (! error)
                    byDevice[target.string()] = entry.path().string();
            }

            return byDevice;
        }

        /*  The path a camera's identifier opens. */
        std::string pathOf (const std::string& id)
        {
            return id.substr (std::strlen (idPrefix));
        }

        bool captures (int fd, std::string& name)
        {
            v4l2_capability capability {};

            if (control (fd, VIDIOC_QUERYCAP, &capability) != 0)
                return false;

            const auto caps = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0 ? capability.device_caps : capability.capabilities;
            name = reinterpret_cast<const char*> (capability.card);
            return (caps & V4L2_CAP_VIDEO_CAPTURE) != 0 && (caps & V4L2_CAP_STREAMING) != 0;
        }

        struct Offer
        {
            std::uint32_t pixelFormat = 0;
            std::uint32_t width = 0;
            std::uint32_t height = 0;
            std::uint32_t numerator = 0;     ///< the interval: numerator / denominator seconds
            std::uint32_t denominator = 0;
        };

        /*  EVERY SIZE AND RATE IT OFFERS IN YUYV OR MJPEG, by word, YUYV first. */
        std::map<std::string, Offer> offersOf (int fd, std::vector<std::string>& order)
        {
            std::map<std::string, Offer> offers;

            for (const auto pixelFormat : { static_cast<std::uint32_t> (V4L2_PIX_FMT_YUYV), static_cast<std::uint32_t> (V4L2_PIX_FMT_MJPEG) })
            {
                bool offered = false;
                v4l2_fmtdesc description {};
                description.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

                for (description.index = 0; control (fd, VIDIOC_ENUM_FMT, &description) == 0; ++description.index)
                    offered = offered || description.pixelformat == pixelFormat;

                if (! offered)
                    continue;

                v4l2_frmsizeenum size {};
                size.pixel_format = pixelFormat;

                for (size.index = 0; control (fd, VIDIOC_ENUM_FRAMESIZES, &size) == 0; ++size.index)
                {
                    //  A range of sizes: its largest, which is what a camera that lists so means.
                    const auto width = size.type == V4L2_FRMSIZE_TYPE_DISCRETE ? size.discrete.width : size.stepwise.max_width;
                    const auto height = size.type == V4L2_FRMSIZE_TYPE_DISCRETE ? size.discrete.height : size.stepwise.max_height;

                    v4l2_frmivalenum interval {};
                    interval.pixel_format = pixelFormat;
                    interval.width = width;
                    interval.height = height;

                    for (interval.index = 0; control (fd, VIDIOC_ENUM_FRAMEINTERVALS, &interval) == 0; ++interval.index)
                    {
                        const auto& fraction = interval.type == V4L2_FRMIVAL_TYPE_DISCRETE ? interval.discrete : interval.stepwise.min;

                        if (fraction.numerator == 0 || fraction.denominator == 0)
                            continue;

                        modes::Format format;
                        format.width = static_cast<int> (width);
                        format.height = static_cast<int> (height);
                        format.frameDuration = fraction.numerator;
                        format.timeScale = fraction.denominator;
                        const auto word = modes::wordFor (format);

                        if (word.empty() || offers.count (word) != 0)
                            continue;

                        offers[word] = { pixelFormat, width, height, fraction.numerator, fraction.denominator };
                        order.push_back (word);

                        if (interval.type != V4L2_FRMIVAL_TYPE_DISCRETE)
                            break;
                    }

                    if (size.type != V4L2_FRMSIZE_TYPE_DISCRETE)
                        break;
                }
            }

            return offers;
        }

        //======================================================================
        class V4l2Source final : public Source
        {
        public:
            V4l2Source (int fdToOwn, std::string wordToUse, std::vector<std::string> formatsToUse)
                : fd (fdToOwn), word (std::move (wordToUse)), offered (std::move (formatsToUse))
            {
            }

            bool begin (const Offer& offer, std::string& why)
            {
                v4l2_format format {};
                format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                format.fmt.pix.width = offer.width;
                format.fmt.pix.height = offer.height;
                format.fmt.pix.pixelformat = offer.pixelFormat;
                format.fmt.pix.field = V4L2_FIELD_ANY;

                if (control (fd, VIDIOC_S_FMT, &format) != 0)
                {
                    why = errno == EBUSY ? std::string ("another program has the camera") : "the camera would not send " + word;
                    return false;
                }

                width = static_cast<int> (format.fmt.pix.width);
                height = static_cast<int> (format.fmt.pix.height);
                rowBytes = static_cast<int> (format.fmt.pix.bytesperline);
                pixelFormat = format.fmt.pix.pixelformat;
                fullRange = format.fmt.pix.quantization == V4L2_QUANTIZATION_FULL_RANGE;

                v4l2_streamparm rate {};
                rate.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                rate.parm.capture.timeperframe.numerator = offer.numerator;
                rate.parm.capture.timeperframe.denominator = offer.denominator;
                control (fd, VIDIOC_S_PARM, &rate);

                v4l2_requestbuffers request {};
                request.count = 4;
                request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                request.memory = V4L2_MEMORY_MMAP;

                if (control (fd, VIDIOC_REQBUFS, &request) != 0 || request.count < 2)
                {
                    why = "the camera would not lend its buffers";
                    return false;
                }

                for (std::uint32_t n = 0; n < request.count; ++n)
                {
                    v4l2_buffer buffer {};
                    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    buffer.memory = V4L2_MEMORY_MMAP;
                    buffer.index = n;

                    if (control (fd, VIDIOC_QUERYBUF, &buffer) != 0)
                    {
                        why = "the camera would not lend its buffers";
                        return false;
                    }

                    auto* data = mmap (nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, static_cast<off_t> (buffer.m.offset));

                    if (data == MAP_FAILED)
                    {
                        why = "the camera's buffers could not be mapped";
                        return false;
                    }

                    mapped.push_back ({ data, buffer.length });

                    if (control (fd, VIDIOC_QBUF, &buffer) != 0)
                    {
                        why = "the camera would not take its buffers back";
                        return false;
                    }
                }

                auto type = static_cast<int> (V4L2_BUF_TYPE_VIDEO_CAPTURE);

                if (control (fd, VIDIOC_STREAMON, &type) != 0)
                {
                    why = errno == EBUSY ? std::string ("another program has the camera") : std::string ("the camera would not start");
                    return false;
                }

                streaming = true;
                reading = std::thread ([this] { read(); });
                return true;
            }

            ~V4l2Source() override
            {
                stopping.store (true);

                if (reading.joinable())
                    reading.join();

                if (streaming)
                {
                    auto type = static_cast<int> (V4L2_BUF_TYPE_VIDEO_CAPTURE);
                    control (fd, VIDIOC_STREAMOFF, &type);
                }

                for (const auto& [data, length] : mapped)
                    munmap (data, length);

                close (fd);
            }

            bool newest (std::vector<std::uint8_t>& bgra, int& w, int& h, std::uint64_t& serial) override
            {
                const std::scoped_lock hold (lock);

                if (frameSerial == serial || latest.empty())
                    return false;

                bgra = latest;
                w = width;
                h = height;
                serial = frameSerial;
                return true;
            }

            bool signal() override
            {
                const std::scoped_lock hold (lock);
                return trouble.empty() && arrivedLately (arrivedAt);
            }

            std::string formatWord() override             { return word; }

            std::string problem() override
            {
                const std::scoped_lock hold (lock);
                return trouble;
            }

            std::vector<std::string> formats() override   { return offered; }

        private:
            void read()
            {
                std::vector<std::uint8_t> frame;

                while (! stopping.load())
                {
                    pollfd waiting { fd, POLLIN, 0 };

                    if (poll (&waiting, 1, 100) <= 0)
                        continue;

                    v4l2_buffer buffer {};
                    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    buffer.memory = V4L2_MEMORY_MMAP;

                    if (control (fd, VIDIOC_DQBUF, &buffer) != 0)
                    {
                        if (errno == EAGAIN)
                            continue;

                        const std::scoped_lock hold (lock);
                        trouble = "the camera stopped: unplugged, or taken by another program";
                        return;
                    }

                    const auto* data = static_cast<const std::uint8_t*> (mapped[buffer.index].first);
                    frame.resize (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4);
                    auto turned = true;

                    if (pixelFormat == V4L2_PIX_FMT_YUYV)
                        yuyvToBgra (data, width, height, rowBytes, frame.data(), fullRange);
                    else
                        turned = decodeJpeg (data, buffer.bytesused, frame);

                    control (fd, VIDIOC_QBUF, &buffer);

                    if (! turned)
                        continue;

                    const std::scoped_lock hold (lock);
                    latest.swap (frame);
                    ++frameSerial;
                    arrivedAt = nowNanos();
                    trouble.clear();
                }
            }

            /*  An MJPEG frame through JUCE's JPEG reader, into BGRA at its size. */
            bool decodeJpeg (const std::uint8_t* data, std::size_t length, std::vector<std::uint8_t>& frame) const
            {
                auto image = juce::ImageFileFormat::loadFrom (data, length);

                if (! image.isValid() || image.getWidth() != width || image.getHeight() != height)
                    return false;

                image = image.convertedToFormat (juce::Image::ARGB);
                const juce::Image::BitmapData pixels (image, juce::Image::BitmapData::readOnly);

                for (int y = 0; y < height; ++y)
                {
                    auto* to = frame.data() + static_cast<std::size_t> (y) * static_cast<std::size_t> (width) * 4;
                    std::memcpy (to, pixels.getLinePointer (y), static_cast<std::size_t> (width) * 4);

                    for (std::size_t x = 3; x < static_cast<std::size_t> (width) * 4; x += 4)
                        to[x] = 255;
                }

                return true;
            }

            const int fd;
            const std::string word;
            const std::vector<std::string> offered;
            int width = 0;
            int height = 0;
            int rowBytes = 0;
            std::uint32_t pixelFormat = 0;
            bool fullRange = false;
            bool streaming = false;
            std::vector<std::pair<void*, std::size_t>> mapped;
            std::thread reading;
            std::atomic<bool> stopping { false };

            std::mutex lock;
            std::vector<std::uint8_t> latest;
            std::uint64_t frameSerial = 0;
            std::int64_t arrivedAt = 0;
            std::string trouble;
        };

        //======================================================================
        class V4l2Cameras final : public System
        {
        public:
            Found find() override
            {
                Found found;
                const auto stable = stableNames();

                for (int n = 0; n < 64; ++n)
                {
                    const auto path = "/dev/video" + std::to_string (n);
                    const auto fd = ::open (path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);

                    if (fd < 0)
                        continue;

                    std::string name;
                    const auto camera = captures (fd, name);
                    close (fd);

                    if (! camera)
                        continue;

                    const auto byId = stable.find (path);
                    Camera listed;
                    listed.id = idPrefix + (byId != stable.end() ? byId->second : path);
                    listed.name = name;

                    const std::scoped_lock hold (lock);

                    if (const auto known = formatsKnown.find (listed.id); known != formatsKnown.end())
                        listed.formats = known->second;

                    found.cameras.push_back (std::move (listed));
                }

                return found;
            }

            std::unique_ptr<Source> open (const std::string& id, const std::string& format, std::string& why) override
            {
                if (! isCamera (id))
                {
                    why = "the camera is not connected";
                    return nullptr;
                }

                const auto fd = ::open (pathOf (id).c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);

                if (fd < 0)
                {
                    why = errno == EACCES ? std::string ("Go.dot may not open the camera: the user needs to be in the video group")
                                          : std::string ("the camera is not connected");
                    return nullptr;
                }

                std::string name;

                if (! captures (fd, name))
                {
                    close (fd);
                    why = "the camera is not connected";
                    return nullptr;
                }

                std::vector<std::string> order;
                const auto offers = offersOf (fd, order);

                {
                    const std::scoped_lock hold (lock);
                    formatsKnown[id] = order;
                }

                const auto word = format == modes::automatic || format.empty() ? chooseAutomatic (order) : format;
                const auto chosen = offers.find (word);

                if (chosen == offers.end())
                {
                    close (fd);
                    why = word.empty() ? std::string ("the camera offers no format Go.dot reads (YUYV or MJPEG)")
                                       : "the camera does not offer " + word;
                    return nullptr;
                }

                auto source = std::make_unique<V4l2Source> (fd, word, order);

                if (! source->begin (chosen->second, why))
                    return nullptr;

                return source;
            }

        private:
            std::mutex lock;
            std::map<std::string, std::vector<std::string>> formatsKnown;
        };
    }

    std::unique_ptr<System> makeSystemCameras()
    {
        return std::make_unique<V4l2Cameras>();
    }
}
