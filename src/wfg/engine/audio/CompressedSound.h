/* This file is part of Go.dot — https://github.com/pob31/go.dot
 *
 * Copyright (C) 2026 Pierre-Olivier Boulant
 *
 * Go.dot is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. Go.dot is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * (LICENSE, at the repository root) for more details.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

/*
    A COMPRESSED SOUND IS COPIED INTO THE SHOW AS A WAV (the author,
    2026-10-25: "We can convert all compressed files to wav").

    WHY. An MP3 dropped into a show was copied, analysed, drawn - and never
    played. A cue plays through Tracktion's audio file cache, and the cache
    only ever holds the formats it can map into memory: WAV, AIFF and its own
    float files (tracktion_AudioFileCache.cpp, getOrCreateCachedFile, asks
    `memoryMappedFormatManager`). Tracktion's own answer for everything else is
    a proxy - a WAV it renders beside the clip - and a launcher slot, which is
    what every Go.dot voice plays through, takes no proxy
    (tracktion_EditNodeBuilder.cpp, `assert (role != ClipRole::launcher)`). So
    the arm waited for a mapped reader that could never come, and the run sat
    there reporting nothing. FLAC and Ogg went the same way.

    SO THE SHOW HOLDS WHAT PLAYS. The conversion is at the copy, not behind the
    cue: the file in `media/` is the WAV and the cue names it, so there is no
    second copy to keep in step and nothing hidden to sweep. A compressed file
    a cue names anyway - an older show, an import told not to copy - is
    refused at the arm in so many words (AudioHost's pointSlotAtFile).

    DECODED BY JUCE'S BASIC FORMATS, which is what the analyser reads with, so
    a sound the waveform drew is a sound that converts: MP3 and WMA through
    Media Foundation on Windows; MP3, AAC, M4A and CAF through Core Audio on
    the Mac; FLAC and Ogg Vorbis everywhere. Written as 32-bit float at the
    file's own rate and width: a decoder's output can go past full scale
    between samples, and 24 bits would clip what the file held.
*/

#include <juce_core/juce_core.h>

#include <functional>
#include <string>

namespace wfg::audio
{
    /*  Whether a file's name says a compressed sound - one a cue cannot play
        as it is. By its extension, case-blind, because the window has to know
        the name a file will land under before the worker has opened it. */
    bool isCompressedSound (const std::string& fileName);

    /*  THE NAME IT LANDS UNDER in `media/`: "song.mp3" is "song.wav", and any
        other name is itself. Only the last part of a path is kept. */
    std::string playableNameFor (const std::string& fileName);

    struct WavWritten
    {
        bool ok = false;
        bool unreadable = false;   ///< nothing on this machine decodes it
        std::string why;           ///< what went wrong, as the tail of a sentence
    };

    /*  THE SOUND IN `source`, DECODED, written to `target` as a WAV. The
        samples go into a hidden part-file beside the target and are moved over
        it whole once they are all there, so a conversion that fails or is
        stopped leaves the folder as it found it - the copier's own rule.
        `stopping` is asked between two blocks of samples. Any thread. */
    WavWritten writeAsWav (const juce::File& source, const juce::File& target,
                           const std::function<bool()>& stopping = {});
}
