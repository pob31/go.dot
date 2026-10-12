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
    Timecode rates and labels: what a frame is called, and which frame a name
    means (namespace draft §59, decisions AHF and AHG).

    PURE, and with nothing of JUCE in it, because three sides read it: the
    document's write door judges a cue's `from` by it, the Runner counts a run's
    frames by it on the tick thread, and the audio thread will draw LTC from the
    same numbers (TC.3). One copy of the drop-frame rule, since two copies of it
    are two chances to lose the one subtle line in it.

    A RATE IS A FRACTION, `num / den` frames a second, and never a double: 29.97
    is 30000/1001 exactly, and a code counted in a double drifts from the sound
    it was started beside by a frame an hour. The NOMINAL rate is what the
    labels count to - 24, 25 or 30 - which for the two NTSC rates is not the
    rate the frames go by.

    DROP-FRAME is how 29.97 keeps its labels near the wall clock: the labels
    :00 and :01 are left out at the start of every minute not divisible by ten,
    so a ten-minute block holds 17982 frames and a minute that drops holds 1798.
    Only the NAMES are skipped - no frame is. 23.976 and 29.97 non-drop count
    every label and drift from the wall clock instead, as their readers expect.

    A LABEL IS STORED WITH COLONS WHATEVER THE RATE (AHF): what someone decided,
    readable at the next venue. The semicolon a drop-frame display puts before
    the frames is a way of SHOWING a label, which `formatLabel` does when asked,
    and which the reader takes as well as a colon.

    EVERYTHING WRAPS AT 24 HOURS, as timecode does: the frame after 23:59:59:24
    at 25 is 00:00:00:00.
*/

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace wfg::timecode
{
    /*  The six rates a show may say (AHG), in the order the parameter table
        lists their words. */
    enum class Rate
    {
        fps23976,
        fps24,
        fps25,
        fps2997df,
        fps2997,
        fps30
    };

    /*  Frames a second as a fraction: 24000/1001, 24/1, 25/1, 30000/1001 twice,
        and 30/1. */
    std::int64_t numOf (Rate rate) noexcept;
    std::int64_t denOf (Rate rate) noexcept;

    /*  What the labels count to in a second: 24, 24, 25, 30, 30, 30. */
    int nominalOf (Rate rate) noexcept;

    /*  Whether the labels drop: 29.97 drop-frame alone. */
    bool dropsFrames (Rate rate) noexcept;

    /*  The rate code an MTC full frame and its quarter frames carry: 0 for 24,
        1 for 25, 2 for 29.97 drop-frame, 3 for 30. 23.976 has no code of its
        own and is sent as 24, and 29.97 non-drop as 30, each at its own pace. */
    int mtcCodeOf (Rate rate) noexcept;

    /*  The parameter table's word for a rate - `23.976`, `24`, `25`, `29.97df`,
        `29.97`, `30` - and back. A word the table does not have reads as
        nothing; the caller decides what that means. */
    std::string_view wordOf (Rate rate) noexcept;
    std::optional<Rate> rateFromWord (std::string_view word) noexcept;

    /*  A TIMECODE LABEL, the four numbers a reader shows. Not a frame count:
        which frame a label means depends on the rate, and a drop-frame label
        such as 00:01:00:00 means none at all until it is normalised. */
    struct Label
    {
        int hours = 0;
        int minutes = 0;
        int seconds = 0;
        int frames = 0;

        bool operator== (const Label&) const = default;
    };

    /*  THE ONE READER OF A LABEL. Two digits each for the hours, the minutes,
        the seconds and the frames; a colon or a semicolon between the first
        three, and a colon, a semicolon or a full stop before the frames - the
        spellings a desk, a DAW and a drop-frame display write. Hours 0-23,
        minutes and seconds 0-59, frames below 30, the most any rate counts:
        which frames a rate has is `normalise`'s question, since the label is
        kept whatever the show's rate is and the rate may change under it.
        Anything else reads as nothing. */
    std::optional<Label> parseLabel (std::string_view text) noexcept;

    /*  A label as text, `HH:MM:SS:FF`. With `driftSeparator`, a drop-frame
        rate's label is shown `HH:MM:SS;FF`, as a reader shows one - the run's
        readout uses it; the stored text never does. */
    std::string formatLabel (const Label& label, Rate rate, bool driftSeparator = false);

    /*  How many frames a day holds at a rate: 86400 times the nominal rate, and
        144 ten-minute blocks of 17982 at drop-frame. */
    std::int64_t framesPerDay (Rate rate) noexcept;

    /*  The frame a label names, counted from 00:00:00:00, at a rate - the label
        normalised first, so a name the rate lacks is the frame after it. */
    std::int64_t frameOf (const Label& label, Rate rate) noexcept;

    /*  The label of a frame at a rate, the frame wrapped into one day first: a
        negative one counts back from midnight. */
    Label labelOf (std::int64_t frame, Rate rate) noexcept;

    /*  A LABEL THE RATE LACKS BECOMES THE NEXT ONE THERE IS (AHF): frames past
        the nominal rate carry into the next second, and at drop-frame the two
        names each dropping minute leaves out become its :02. A label the rate
        has is returned as it is. */
    Label normalise (const Label& label, Rate rate) noexcept;
}
