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
    A CAMERA, AS THE RENDERER SEES ONE (namespace draft §72, APS).

    A webcam, a USB capture dongle, an ATEM Mini or a Web Presenter showing up
    as a webcam: whatever the system lists as a camera, through its own
    framework - Media Foundation on Windows (Camera_mf.cpp), AVFoundation on
    macOS (Camera_avf.mm), Video4Linux on Linux (Camera_v4l2.cpp) - or the TEST
    CAMERA (Camera_fake.cpp), which WFG_CAMERA_FAKE=1 puts in their place and
    every test uses. Nothing here names any of them.

    A camera is a "Video capture" input (APR), beside a card's input: its
    identifier starts with `cam:` and the rest is the system's own - Media
    Foundation's symbolic link, AVFoundation's unique ID, the /dev/v4l/by-id
    link - so it is the same camera tomorrow in another port.

    Pictures only (APU): a camera's sound is not taken.

    LISTED WITHOUT LIGHTING: `find` gives names, never formats, so no camera
    switches on to be listed; a camera's formats are known once it has been
    opened, and said from then on.

    THREADS. Any thread; an opened camera reads on a thread of its own and hands
    over its newest frame, BGRA, when asked.
*/

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wfg::video::camera
{
    /*  A camera, as listed: its identifier (with `cam:`), its name, and the
        formats it offers as words (VideoModes.h) once it has been opened. */
    struct Camera
    {
        std::string id;
        std::string name;
        std::vector<std::string> formats;
    };

    struct Found
    {
        std::vector<Camera> cameras;
        std::string problem;    ///< why none can be listed - not why there are none
    };

    /*  AN OPENED CAMERA, reading on a thread of its own. */
    class Source
    {
    public:
        virtual ~Source() = default;

        /*  The newest frame, BGRA, top row first, `width * 4` a row, when it
            is newer than `serial`; `serial` moves to it. */
        virtual bool newest (std::vector<std::uint8_t>& bgra, int& width, int& height, std::uint64_t& serial) = 0;

        /*  Whether pictures arrive, the format they arrive in, and why not. */
        virtual bool signal() = 0;
        virtual std::string formatWord() = 0;
        virtual std::string problem() = 0;

        /*  What the camera offers, as words, now that it is open. */
        virtual std::vector<std::string> formats() = 0;
    };

    class System
    {
    public:
        virtual ~System() = default;

        /*  The cameras the system lists now: names only, a moment's work. */
        virtual Found find() = 0;

        /*  `id` opened at `format` - a word it offers, or `auto`: its
            largest size at 25 frames a second or more, else its largest
            (`chooseAutomatic`). Null and why when it cannot be: no such
            camera, another program has it, the system will not let Go.dot
            use cameras. */
        virtual std::unique_ptr<Source> open (const std::string& id, const std::string& format, std::string& why) = 0;
    };

    /*  The cameras this process reaches: the test camera when
        WFG_CAMERA_FAKE is 1, the system's otherwise - made once, kept. */
    System& system();

    std::unique_ptr<System> makeSystemCameras();
    std::unique_ptr<System> makeFakeCameras();

    /*  ONE INPUT PER CAMERA (APZ): `cameras`, refusing a camera already
        opened through it until that Source has gone - which Windows, sharing
        a camera between programs, would not refuse by itself. `system()` is
        made so. */
    std::unique_ptr<System> oneInputEach (std::unique_ptr<System> cameras);

    /*  An identifier is a camera's when it starts so. */
    inline constexpr const char* idPrefix = "cam:";
    bool isCamera (const std::string& id) noexcept;

    /*  Of `formats` (words), the one `auto` opens: the largest picture at 25
        frames a second or more, else the largest; ties by the faster rate. */
    std::string chooseAutomatic (const std::vector<std::string>& formats);

    /*  QUIET A SECOND IS NO SIGNAL: a camera whose newest frame is older than
        a second - a virtual camera given no picture, a dongle with no cable
        in - says no signal and draws black, without being let go; one that
        stopped says so in `problem` and is opened again. Every source stamps
        its frames with `nowNanos`, the steady clock. */
    std::int64_t nowNanos() noexcept;
    bool arrivedLately (std::int64_t arrivedAt) noexcept;

    /*  The test camera's identifier and name, and its quiet twin's, which
        opens and sends nothing. */
    inline constexpr const char* fakeId = "cam:fake-1";
    inline constexpr const char* fakeName = "Go.dot test camera";
    inline constexpr const char* fakeQuietId = "cam:fake-2";
    inline constexpr const char* fakeQuietName = "Go.dot test camera, no picture";

    /*  YUYV (Y U Y V, two pixels in four bytes) into BGRA, BT.601 below 720
        lines and BT.709 from it, at legal levels or full range as the camera
        says: what Video4Linux hands over most often. */
    void yuyvToBgra (const std::uint8_t* yuyv, int width, int height, int rowBytes, std::uint8_t* bgra, bool fullRange) noexcept;
}
