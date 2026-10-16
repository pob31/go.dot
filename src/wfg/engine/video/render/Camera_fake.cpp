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
    THE TEST CAMERA (namespace draft §72): one camera, "Go.dot test camera",
    offering 720p30, 1080p30 and 640x480p30, its picture a grey ground with a
    white bar stepping across, a frame each thirtieth of a second on the
    machine's clock. It keeps no thread: each ask brings it up to now. One
    input at a time is oneInputEach's (Camera.cpp), as for a real camera.

    And its QUIET TWIN, "Go.dot test camera, no picture": it opens and sends
    nothing, as a virtual camera given no picture does - no signal, nothing
    wrong.
*/

#include <wfg/engine/video/render/Camera.h>

#include <wfg/engine/video/VideoModes.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <string_view>

namespace wfg::video::camera
{
    namespace
    {
        const std::vector<std::string>& fakeFormats()
        {
            static const std::vector<std::string> offered { "720p30", "1080p30", "640x480p30" };
            return offered;
        }

        std::int64_t steadyNanos()
        {
            return std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        struct Shared
        {
            std::mutex lock;
            std::set<std::string> opened;    ///< opened once, so their formats are known
        };

        class FakeSource final : public Source
        {
        public:
            FakeSource (std::string wordToUse, modes::Format formatToUse, bool quietToUse)
                : word (std::move (wordToUse)), format (formatToUse), quiet (quietToUse), began (steadyNanos())
            {
            }

            bool newest (std::vector<std::uint8_t>& bgra, int& width, int& height, std::uint64_t& serial) override
            {
                if (quiet)
                    return false;

                const auto period = 1.0e9 / std::max (1.0, format.frameRate());
                const auto frame = static_cast<std::uint64_t> (static_cast<double> (steadyNanos() - began) / period) + 1;

                if (frame == serial)
                    return false;

                width = format.width;
                height = format.height;
                bgra.assign (static_cast<std::size_t> (width) * static_cast<std::size_t> (height) * 4, 96);

                //  A white bar, sixteen pixels a frame, the full height.
                const auto bar = std::max (4, width / 40);
                const auto x0 = static_cast<int> ((frame * 16) % static_cast<std::uint64_t> (std::max (1, width - bar)));

                for (int y = 0; y < height; ++y)
                    std::memset (bgra.data() + (static_cast<std::size_t> (y) * static_cast<std::size_t> (width) + static_cast<std::size_t> (x0)) * 4,
                                 255, static_cast<std::size_t> (bar) * 4);

                for (std::size_t at = 3; at < bgra.size(); at += 4)
                    bgra[at] = 255;

                serial = frame;
                return true;
            }

            bool signal() override                        { return ! quiet; }
            std::string formatWord() override             { return word; }
            std::string problem() override                { return {}; }
            std::vector<std::string> formats() override   { return fakeFormats(); }

        private:
            const std::string word;
            const modes::Format format;
            const bool quiet;
            const std::int64_t began;
        };

        class FakeCameras final : public System
        {
        public:
            Found find() override
            {
                Found found;
                const std::scoped_lock hold (shared->lock);

                for (const auto* id : { fakeId, fakeQuietId })
                {
                    Camera camera;
                    camera.id = id;
                    camera.name = std::string_view (id) == fakeId ? fakeName : fakeQuietName;

                    if (shared->opened.count (id) != 0)
                        camera.formats = fakeFormats();

                    found.cameras.push_back (std::move (camera));
                }

                return found;
            }

            std::unique_ptr<Source> open (const std::string& id, const std::string& format, std::string& why) override
            {
                if (id != fakeId && id != fakeQuietId)
                {
                    why = "the camera is not connected";
                    return nullptr;
                }

                const auto word = format == modes::automatic || format.empty() ? chooseAutomatic (fakeFormats()) : format;

                if (std::find (fakeFormats().begin(), fakeFormats().end(), word) == fakeFormats().end())
                {
                    why = "the camera does not offer " + word;
                    return nullptr;
                }

                const std::scoped_lock hold (shared->lock);
                shared->opened.insert (id);
                return std::make_unique<FakeSource> (word, *modes::parse (word), id == fakeQuietId);
            }

        private:
            const std::shared_ptr<Shared> shared = std::make_shared<Shared>();
        };
    }

    std::unique_ptr<System> makeFakeCameras()
    {
        return std::make_unique<FakeCameras>();
    }
}
