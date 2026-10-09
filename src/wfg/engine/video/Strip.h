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
    A MOVIE'S STRIP: WHERE ITS SHOTS CHANGE, AND SMALL PICTURES ALONG IT
    (namespace draft §47, AAI).

    The author, 2026-10-09: "Could we try to have scene detection and show a
    strip of thumbnails an equivalent manner to the audio waveform?" A sound's
    panel draws its waveform; a movie's draws this - its pictures across its
    length, a mark at every cut - and an in or out point dragged near a cut
    lands on it.

    A FRAME'S SIGNATURE is what a cut is judged by: the frame seen as sixteen
    by nine squares of colour, and how its brightness is spread over
    thirty-two steps. Two frames' distance is half how far their squares
    differ and half how far their brightness moved, nought to one - so a camera
    move or a light fading changes it a little a frame, and a cut a lot at once.

    A CUT is a frame whose distance from the one before stands out from the
    frames around it: above an eighth, and above three times the middle of the
    distances within a second either side. A shot shorter than four tenths of
    a second is not believed unless the change is stark; a single frame that
    differs from both its neighbours while they agree - a flash, a strobe - is
    no cut at all, going or coming back. A dissolve changes a little a frame for several frames, and
    is found by what it adds up to: a run of raised distances whose ends
    differ by more than three tenths is a cut, marked gradual, at its middle.

    THE PICTURES: one at the first frames of every shot and one every so often
    between - every second, or a three-hundredth of the movie, whichever is
    longer - at most 240, each 80 pixels across.

    KEPT beside the analysis of sounds, `<key>.tms` in the media's `.timbre`
    folder, in a format of its own with a checksum; read again when the show
    opens. std only: a strip can be found in a test from colours made up.
*/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace wfg::video::strip
{
    constexpr int gridWidth = 16;
    constexpr int gridHeight = 9;
    constexpr int histogramBins = 32;

    //  The grid each signature is sampled at: four points a square each way.
    constexpr int sampleWidth = gridWidth * 4;
    constexpr int sampleHeight = gridHeight * 4;

    constexpr int thumbnailWidth = 80;
    constexpr std::size_t maxThumbnails = 240;

    struct Signature
    {
        std::array<std::uint8_t, gridWidth * gridHeight * 3> grid {};
        std::array<float, histogramBins> histogram {};
    };

    /*  A FRAME'S SIGNATURE from what it shows at (u, v) - nought to one, from
        its top-left - each channel 0..255; false where nothing is known. */
    using Sampler = std::function<bool (double u, double v, double& red, double& green, double& blue)>;

    Signature signatureOf (const Sampler& sample);

    /** How unlike two frames are: nought the same, one as unlike as can be. */
    double distance (const Signature& a, const Signature& b) noexcept;

    struct Cut
    {
        double seconds = 0.0;       ///< where the new shot starts
        double strength = 0.0;      ///< the distance that made it, nought to one
        bool gradual = false;       ///< a dissolve, placed at its middle
    };

    /*  THE CUTS IN A RUN OF FRAMES, fed in order - every frame, or every few,
        at even steps - and asked for at the end. */
    class CutFinder
    {
    public:
        void add (double seconds, const Signature& signature);
        std::vector<Cut> finish() const;

        std::size_t frames() const noexcept { return seen.size(); }

    private:
        struct Seen
        {
            double seconds = 0.0;
            Signature signature;
        };

        std::vector<Seen> seen;
    };

    /*  A CUT FOUND BETWEEN TWO FRAMES FED, `before` and `after` - indices of
        the movie's own frames - placed on its first frame: the frame between
        them nearer `after`'s signature than `before`'s, by halving. `at` gives
        a frame's signature; asked about log2 of the gap times. */
    int firstFrameOfShot (int before, int after, const std::function<Signature (int frame)>& at);

    struct Thumbnail
    {
        double seconds = 0.0;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgb;      ///< rows from the top-left, three bytes a pixel
    };

    struct MovieStrip
    {
        double duration = 0.0;
        int width = 0;                      ///< the movie's own size
        int height = 0;
        std::vector<Cut> cuts;
        std::vector<Thumbnail> thumbnails;  ///< in the order of their seconds

        /** The picture nearest at or before `seconds`, or nullptr with none. */
        const Thumbnail* thumbnailAt (double seconds) const noexcept;
    };

    /*  WHEN THE PICTURES ARE TAKEN: just inside the start of every shot, the
        movie's own start, and every so often between - at most `maxThumbnails`. */
    std::vector<double> thumbnailTimes (double duration, const std::vector<Cut>& cuts);

    /** A thumbnail's height for a movie of this shape, 80 across. */
    int thumbnailHeightFor (int movieWidth, int movieHeight) noexcept;

    /*  THE CACHE'S BYTES, and back: false, with `out` untouched, for bytes that
        are not a strip of this version or whose checksum fails. */
    std::vector<std::uint8_t> encode (const MovieStrip& strip);
    bool decode (const std::uint8_t* bytes, std::size_t size, MovieStrip& out);

    /** Bumped whenever finding or keeping a strip changes. */
    constexpr std::uint32_t formatVersion = 1;
}
