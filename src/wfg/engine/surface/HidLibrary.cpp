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

#include <wfg/engine/surface/HidLibrary.h>

#include <hidapi/hidapi/hidapi.h>

#include <mutex>

namespace wfg::surface
{
    namespace
    {
        std::mutex& guard()
        {
            static std::mutex held;
            return held;
        }

        int& holders()
        {
            static int count = 0;
            return count;
        }
    }

    HidLibrary::HidLibrary()
    {
        const std::lock_guard<std::mutex> held { guard() };

        if (holders()++ == 0)
            hid_init();
    }

    HidLibrary::~HidLibrary()
    {
        const std::lock_guard<std::mutex> held { guard() };

        if (--holders() == 0)
            hid_exit();
    }
}
