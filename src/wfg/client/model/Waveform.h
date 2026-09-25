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
    What a run is playing, as a bar one column wide per pixel.

    THE ENGINE ALREADY ANSWERED THIS QUESTION (PRD §3.30, PR 5.7). Every file
    the show names is analysed on its own thread into a `TimbrePyramid`: level 0
    is one frame per hop, and each level above it is half the one below, down to
    sixty-four frames. `Timbre.h` says in as many words what this file is for -
    *"a client drawing a bar picks the level whose frame count is nearest its
    pixel count and reads it once, so no redraw recomputes anything"*.

    SO THERE IS NO SIGNAL PROCESSING HERE and there must never be any. A window
    that decided for itself what a file looks like would be a second answer to a
    question the engine has already answered, and the two would drift the first
    time the ramp moved. This picks a level, buckets it into columns and stops.

    FOUR NUMBERS PER COLUMN, and the colour three of them make is the show's own
    (§3.30, §4.8): hue from the spectral centroid along the fixed ramp,
    saturation from one minus the spectral flatness - so noise is grey and a
    tone is vivid - and lightness rising with the centroid. The fourth, the
    peak, is what makes it read as a WAVEFORM rather than as a stripe: the
    column's height. Somebody who cannot sort the hues still sees the shape of
    the sound, which is §4.8's rule applied to a picture rather than to a word.

    std only, like the rest of model/: `Timbre.h` and `MediaInfo.h` are both
    std-only by their own design, so a pyramid can be built in a test and asked
    for its columns with no window and no engine running.
*/

#include <cstdint>
#include <string>
#include <vector>

namespace wfg::audio { struct TimbrePyramid; struct PeakTrack; }

namespace wfg::client::model
{
    /** One column of a bar: what that slice of the file sounds like, and how loud. */
    struct Column
    {
        double hue = 0.0;          ///< degrees, [0, 360)
        double saturation = 0.0;   ///< [0, 1]; nought is noise, one is a tone
        double lightness = 0.0;    ///< [0, 1]; nought is silence
        double peak = 0.0;         ///< [0, 1]; the column's height

        /*  THE WAVE'S OWN SHAPE, when the finer level is there (Peaks.h,
            2026-09-25): the lowest and the highest sample the column holds, in
            [-1, 1]. Without it, the mirror of the peak: -peak and peak. */
        double low = 0.0;
        double high = 0.0;
    };

    /*  WHICH LEVEL OF THE PYRAMID A BAR THIS WIDE SHOULD READ: the coarsest one
        that still has at least a frame per column, so the bucketing below
        averages over a handful of frames rather than over thousands - and the
        finest level when even that is not enough, which is a bar wider than the
        file is long in frames.

        Answered separately from `waveform` because it is the part worth
        asserting: a bar that silently read level 0 of a three-hour show would
        still LOOK right and would walk a million frames per repaint. */
    std::size_t levelFor (const audio::TimbrePyramid& pyramid, int width);

    /*  THE BAR, one column per pixel, left to right across the whole file.

        Empty when there is nothing to draw - no pyramid, no frames, or a width
        of nought - which a caller reads as "not analysed yet" and draws as the
        engine's own answer for that case: grey, and no shape (§3.30). A file
        being analysed is not a file with no sound in it, and a bar that
        invented a flat line for one would be saying something false about a
        cue somebody is about to fire.

        Each column takes the LOUDEST frame of its span rather than the mean of
        them: a transient that survives to the screen is what an operator is
        looking for when they glance at this, and a mean would flatten every
        attack in a long file into the same grey. The pyramid's own levels
        average; this one does not average twice. */
    std::vector<Column> waveform (const audio::TimbrePyramid& pyramid, int width);

    /*  THE SAME BAR OVER A WINDOW OF THE FILE rather than the whole of it,
        which is what makes an editor zoom. `from` and `to` are seconds; a
        window that is empty, backwards or entirely outside the file answers
        with nothing, exactly as the whole-file call does when there is nothing
        to draw.

        IT IS STILL NOT A SECOND ANALYSIS. The level is picked for the number of
        frames the WINDOW holds rather than the file, so zooming in walks down
        the pyramid and reads finer frames of a shorter span - the same cost per
        repaint at every zoom, which is the property the pyramid exists to give
        and the reason a window may be dragged about without the bar stuttering.

        The whole-file overload above is this one over the whole file, and is
        kept because the running pane wants exactly that and should not have to
        know a file's length to ask for it. */
    std::vector<Column> waveform (const audio::TimbrePyramid& pyramid, int width,
                                  double fromSeconds, double toSeconds);

    /*  AND THE SAME BAR WITH ITS HEIGHT FROM THE FINER LEVEL (author,
        2026-09-25: "Can the waveform be more precise in level, not colour
        when zooming in"). The colour is still the pyramid's frame; the height,
        and the low and the high, are the peak track's - sixty-four samples a
        pair and sixteen bits, where a frame was a thousand samples and eight
        bits - picked, as the pyramid's level is, as the coarsest with a pair
        per column. `peaks` null is the call above. */
    std::vector<Column> waveform (const audio::TimbrePyramid& pyramid, const audio::PeakTrack* peaks,
                                  int width, double fromSeconds, double toSeconds);

    /** The whole file, with its finer level when there is one. */
    std::vector<Column> waveform (const audio::TimbrePyramid& pyramid, const audio::PeakTrack* peaks,
                                  int width);

    /** How long the pyramid says the file is, in seconds; nought when it cannot say. */
    double lengthOf (const audio::TimbrePyramid& pyramid);

    /*  WHERE THE PLAYHEAD STANDS, as a fraction of the bar in [0, 1].

        Nought when the length is not known yet, which is the honest answer
        rather than the flattering one: a cue imported in this session has no
        duration until the show is reopened (`MediaInfo::durations()` is frozen
        at load), so a playhead that guessed would slide across a bar it had no
        business measuring. */
    double playhead (double position, double length);

    /*  AND THE SAME THING OVER A WINDOW OF THE FILE rather than over the whole
        of it: where `position` sits between `from` and `to`, in [0, 1].

        THIS IS WHAT MAKES A LOOP VISIBLE. The engine's `position` is a FILE
        position with the range wrap already in it - a looping slice is back at
        its in-point on every pass - so a strip drawn over the stretch the cue
        actually plays shows the head returning to the start of each slice,
        which is what it is doing. Measured against the whole file instead, the
        same jump is a twitch at one end of a picture that is mostly silence
        nobody will hear.

        Nought when the window is empty or backwards, which is the same honest
        answer the whole-file call gives for an unknown length. */
    double playhead (double position, double from, double to);

    /*  AND HOW FAR THROUGH A WAIT A RUN IS, drawn as a bar that empties rather
        than fills (author, 2026-09-18: "pre-waits and post-waits can also have
        progress bars, maybe running the opposite way (right to left) as a
        countdown").

        A COUNTDOWN IS NOT A PROGRESS BAR and the difference is the whole point
        of drawing it backwards: a progress bar answers "how much of this is
        done", which nobody asks about a wait, while a countdown answers "how
        long until this fires", which is the only question an operator has
        while one is running. The number it returns is what is LEFT, in [0, 1],
        so a full bar is a wait that has not started and an empty one is a cue
        about to go.

        One when the total is not known, so an unmeasurable wait reads as
        waiting rather than as finished. */
    double countdown (double remaining, double total);

    /*  WHICH WAY A WAIT'S BAR TRAVELS, which is not the same question for the
        two waits (author, 2026-09-18: "pre (right to left) and post (left to
        right) wait times can be one colour").

        A PRE-WAIT IS TIME UNTIL SOMETHING HAPPENS, so it runs RIGHT TO LEFT and
        is empty at the moment the cue fires: the edge travelling left is the
        thing an operator is watching, and it arrives at the cue. A POST-WAIT is
        time since something happened, so it fills LEFT TO RIGHT and is full when
        the run is done. Same colour, opposite motion - which is the whole of
        what tells them apart at a glance, and is why they are one colour rather
        than two: the motion carries it, so a hue does not have to. */
    bool runsLeftToRight (const std::string& state);
}
