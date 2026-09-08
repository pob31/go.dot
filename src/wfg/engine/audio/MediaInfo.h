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
    How long a media file is, asked once when the show is opened.

    WHY THIS EXISTS AT ALL. Nothing in Go.dot has ever known how long a cue is.
    §3.13's solver cannot work without it - "is this cue still playing at time
    T" is a question about a duration - and the running pane has nothing to draw
    a progress bar against. `/godot/cue/<id>/duration` is the node; this is
    where the number comes from.

    WHY IT IS NOT ASKED OF TRACKTION, which is what the namespace draft first
    said. `te::AudioFile` needs a `te::Engine&`, and there is none at the moment
    a show is read: `wfg tree` and `wfg validate` build no audio at all, and
    `wfg serve` brings the engine up three hundred lines after the document is
    loaded and the first tree snapshot is published. Standing an engine up to
    ask a file's length would also set flush-to-zero on the calling thread for
    the rest of the process, which is a thing this project has already been bitten
    by once and scopes deliberately everywhere else.

    So it is JUCE's own format manager, with the basic formats registered: WAV,
    AIFF, FLAC, Ogg Vorbis, and MP3 only where the build enables it. Tracktion
    reads through the same JUCE readers, so the two agree about every format the
    engine can actually play.

    A FILE THAT WILL NOT READ IS NOUGHT, and that is the answer rather than an
    error: a missing file has failed the ARM and never the load since Phase 2 -
    "a show with one missing sound is still a show somebody has to run tonight" -
    and the same rule holds for one whose header cannot be parsed. The solver
    reads a nought as *I do not know how long this is* and says so in its
    confused list rather than guessing.

    No JUCE type in the signature: the tree and the document read this and
    neither of them names an audio library.
*/

#include <map>
#include <string>

namespace wfg::doc { class ShowDocument; }

namespace wfg::audio
{
    /*  Seconds, or 0.0 when the file is absent, unreadable, or in a format this
        build has no reader for. Never throws. */
    double mediaDurationSeconds (const std::string& absolutePath);

    /*  Every distinct `file` a media cue in this show names, mapped to its
        length in seconds.

        KEYED BY THE PATH THE DOCUMENT WRITES, bundle-relative, rather than by
        cue identifier: two cues naming one file are one read, and a cue whose
        `file` is edited to another file the show already uses gets the right
        answer rather than the previous one's. A `file` edited to something the
        walk never saw reads nought until the show is reloaded, which is the
        same shape as §3.25's graph: what the show declared when it was opened
        is what this session runs on.

        `mediaFolder` is the bundle's `media/` directory; a cue names its file
        relative to it. Reads each file once, on the thread that opens the
        show. */
    std::map<std::string, double> mediaDurations (const doc::ShowDocument& document,
                                                  const std::string& mediaFolder);
}
