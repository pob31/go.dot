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

//  Windows and Linux, where there is no workgroup to share and nobody is ever
//  in one. The Mac's is WorkgroupShare_mac.mm.
#if ! defined (__APPLE__)

#include <wfg/engine/plugin/WorkgroupShare.h>

namespace wfg::plugin
{
    bool shareAudioWorkgroup (const juce::AudioWorkgroup&) { return false; }
    bool shareWorkgroupHandle (void*) { return false; }

    struct WorkgroupFollower::Impl {};

    WorkgroupFollower::WorkgroupFollower (std::int64_t) : impl (std::make_unique<Impl>()) {}
    WorkgroupFollower::~WorkgroupFollower() = default;
    void WorkgroupFollower::poll() {}
    bool WorkgroupFollower::keepUp() noexcept { return false; }
    void WorkgroupFollower::leave() noexcept {}
}

#endif
