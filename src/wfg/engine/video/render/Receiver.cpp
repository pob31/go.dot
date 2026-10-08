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

#include <wfg/engine/video/render/Receiver.h>

namespace wfg::video::render
{
    std::unique_ptr<Receiver> makeReceiver (region::OutputKind kind, const std::string& sender, std::string& why)
    {
        if (sender.empty())
        {
            why = "no sender is named";
            return nullptr;
        }

        switch (kind)
        {
            case region::OutputKind::spout:   return makeSpoutReceiver (sender, why);
            case region::OutputKind::syphon:  return makeSyphonReceiver (sender, why);
            case region::OutputKind::ndi:     return makeNdiReceiver (sender, why);
            case region::OutputKind::display: break;
        }

        why = "a display sends nothing";
        return nullptr;
    }

    std::string discoverSenders()
    {
        return discoverSpout() + discoverSyphon() + discoverNdi();
    }

    void ArrivalRate::arrived (double nowSeconds) noexcept
    {
        if (since < 0.0)
            since = nowSeconds;

        ++counted;
        lastArrival = nowSeconds;

        if (nowSeconds - since >= 1.0)
        {
            lastRate = counted / (nowSeconds - since);
            since = nowSeconds;
            counted = 0;
        }
    }

    double ArrivalRate::age (double nowSeconds) const noexcept
    {
        return lastArrival >= 0.0 ? nowSeconds - lastArrival : 0.0;
    }

    double ArrivalRate::rate (double nowSeconds) const noexcept
    {
        //  Nothing for two seconds is nothing arriving, whatever the last count said.
        return lastArrival >= 0.0 && nowSeconds - lastArrival < 2.0 ? lastRate : 0.0;
    }

   #if ! defined (_WIN32)
    std::unique_ptr<Receiver> makeSpoutReceiver (const std::string&, std::string& why)
    {
        why = "Spout is Windows's: this system has none";
        return nullptr;
    }

    std::string discoverSpout()  { return {}; }
   #endif

   #if ! defined (__APPLE__)
    std::unique_ptr<Receiver> makeSyphonReceiver (const std::string&, std::string& why)
    {
        why = "Syphon is the Mac's: this system has none";
        return nullptr;
    }

    std::string discoverSyphon()  { return {}; }
   #endif
}
