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
    A PICTURE ANOTHER PROGRAM SENDS, TAKEN IN (namespace draft §44, YB, YD): a
    video input's receiver, on the render thread, made while the input is
    declared and enabled - never at GO - and asked each frame for the newest
    picture that arrived. SPOUT on Windows and SYPHON on macOS hand over a
    texture on the renderer's own device, which sokol is given as it is; NDI
    receives on threads of its own and the newest frame is uploaded once a
    frame however many cues show it.

    A live picture is not on the audio clock (YD): it is shown as it comes.
    Its picture's first row is its top, whatever the system.
*/

#include <wfg/engine/video/VideoRegion.h>

#include <sokol/sokol_gfx.h>

#include <memory>
#include <string>

namespace wfg::video::render
{
    class Receiver
    {
    public:
        virtual ~Receiver() = default;

        /*  The newest picture that arrived, taken in: once a frame, before
            anything is drawn. */
        virtual void update() = 0;

        /*  The picture to draw - invalid while none has arrived - and its
            size. */
        virtual sg_view picture() const = 0;
        virtual int width() const = 0;
        virtual int height() const = 0;

        /*  Whether pictures arrive, how many a second lately, and why not. */
        virtual bool connected() const = 0;
        virtual double frameRate() const = 0;
        virtual std::string problem() const = 0;

        /*  How long ago the last picture arrived, in seconds; nought before
            any has. An insert's round trip shows here (YH). */
        virtual double age() const = 0;
    };

    /*  A receiver of `kind` for what `sender` sends; null and why where this
        system has not got that kind, or NDI's runtime is not installed. Render
        thread, the device open. */
    std::unique_ptr<Receiver> makeReceiver (region::OutputKind kind, const std::string& sender, std::string& why);

    /*  WHAT OTHER PROGRAMS OFFER NOW, as `videoInputs/available` says it: the
        kind, a tab, the name, a line each. Render thread; a moment's work, so
        asked once a second. */
    std::string discoverSenders();

    /*  Each system's own. */
    std::unique_ptr<Receiver> makeSpoutReceiver (const std::string& sender, std::string& why);
    std::unique_ptr<Receiver> makeSyphonReceiver (const std::string& sender, std::string& why);
    std::unique_ptr<Receiver> makeNdiReceiver (const std::string& sender, std::string& why);
    std::string discoverSpout();
    std::string discoverSyphon();
    std::string discoverNdi();

    /*  A FRAME RATE FROM ARRIVALS: frames counted against the clock, read as a
        rate over the last second or so. */
    class ArrivalRate
    {
    public:
        void arrived (double nowSeconds) noexcept;
        double rate (double nowSeconds) const noexcept;
        double age (double nowSeconds) const noexcept;

    private:
        double since = -1.0;
        int counted = 0;
        double lastRate = 0.0;
        double lastArrival = -1.0;
    };
}
