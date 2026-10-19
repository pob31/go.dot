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
    HIDAPI, HELD BY WHOEVER USES IT (namespace draft §95).

    hidapi has one `hid_init` and one `hid_exit` for the whole process, and
    `hid_exit` frees what every open device depends on. Two readers - the
    SpaceMouse's and the Stream Decks' - each starting and stopping on their
    own would pull the library out from under the other. So each holds it
    while its thread runs: the first hold initialises it, the last let-go
    exits it.

    Any thread; std only behind the header.
*/

namespace wfg::surface
{
    class HidLibrary
    {
    public:
        HidLibrary();
        ~HidLibrary();

        HidLibrary (const HidLibrary&) = delete;
        HidLibrary& operator= (const HidLibrary&) = delete;
    };
}
