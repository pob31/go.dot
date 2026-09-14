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
    The pyramids, over HTTP: the one route on the tree's port that is not an
    address (namespace draft §14.5, PR 5.8).

    A ROUTE AND NOT A NODE, because the tree carries what is true now and this
    carries what is true always. A run's colour at its playhead is a parameter,
    and lives at `/godot/run/<id>/timbre`. A file's whole pyramid is kilobytes
    of frames that do not change while the file does not, and has no value at
    a moment: as a level in the tree it would ride on every description a
    client polls for, ten times a second, for ever, and the poll would grow by
    the size of the show's media. So a client reads a media cue's hash once, at
    `/godot/cue/<id>/hash`, and fetches the level it wants from here, once:

        GET /media/<sha256>/timbre?INFO       the header, as JSON
        GET /media/<sha256>/timbre?level=N    level N's frames, four bytes each

    `?INFO` is what lets a forty-pixel Gogo bar and a full-width waveform each
    ask for the level they will draw, in one round trip, instead of fetching
    the finest and throwing most of it away - which is the whole reason §3.30
    asked for a pyramid and not a frame array.

    CONTENT-ADDRESSED, AND THAT IS WHAT MAKES IT SAFE. The one part of the path
    that varies is checked to be sixty-four lower-case hex digits and then
    looked up in the records - never joined to a folder - so no request text
    ever becomes a filesystem path, and this needs none of the `isAChildOf`
    test that `/ui` cannot do without.

    BUT ADDRESSED BY THE AUDIO, NOT BY THE ANALYSIS, and that is why a 200 says
    `no-cache` rather than "keep it for a year, it cannot change". The bytes
    behind a URL are the audio's hash run through this build's analysis, and
    the analysis moves: a ramp stop moved in `Timbre.cpp` bumps
    `timbre::formatVersion`, the next build's analyser refuses the old `.tpy`
    and writes the new one under the SAME name, and the same URL answers other
    colours. The author moves those stops while looking at the bar in a
    browser; a year-long, immutable copy would go on showing the old ones, as
    if the move had done nothing. `no-cache` lets a browser store a 200 and
    makes it ask again before reusing it - and since a handler sees no request
    headers, there is no ETag to ask with and no 304 to answer, so asking again
    is fetching again: a level's few kilobytes, per bar, per page load, which
    is cheap beside a wrong colour. A page keeps the levels it has fetched in
    memory for its own life anyway. `?INFO` says which `formatVersion` built
    what it describes, so a client can at least see which analysis it holds.

    IMMUTABILITY CAN COME BACK, with a URL that carries the version beside the
    hash, because such a URL really could never answer two different ways. A
    client has to learn the version before it can build that URL, and `?INFO`
    - which a bar asks first anyway, to choose its level, and which would
    itself stay `no-cache` - now says it. What is not built is the other half:
    a level URL that names the version and is refused when it names another.
    Until something wants the saving, a name that could be wrong for a year is
    worse than a fetch that was not needed.

    A REFUSAL IS NOT KEPT AT ALL. A hash that has not been analysed yet is a
    404 now and a 200 a minute from now, so every error says `no-store`, and a
    browser that asked too early asks again.

    FROM MEMORY, ON A THREAD IT DOES NOT OWN. The server answers this on its
    only HTTP thread, which is also the WebSocket's. A read from the disk here
    - or a wait on the analyser - would stall every subscription push and every
    poll on the port for as long as it took, and on the night the disk is busy
    a browser asking for a colour bar would silence the tablet's whole tree. So
    a request takes ONE snapshot of the records - a pointer copy under a mutex
    the analyser never holds while it works - and builds its answer from the
    pyramid that snapshot keeps alive. The `.tpy` on disk is the analyser's;
    this never opens it, and serves the same bytes from the copy in memory.

    GO.DOT-SHAPED, and so not in OscQueryServer, which matches a prefix and
    knows nothing about what answers behind it: this is the route's side of
    that seam, as `EngineNamespace` is the tree's. Pure - no socket, no thread
    - so a test asks it directly, and once through a running server.

    NOT UNDER `wfg replay`, which has no files to hash and no HTTP server: there
    every timbre node is empty and every bar grey, the same answer §3.30 gives
    for a clip whose cache has not arrived yet.
*/

#include <wfg/engine/oscquery/OscQueryServer.h>

#include <string>

namespace wfg::audio { class MediaInfo; }

namespace wfg::oscquery
{
    /*  Where the route is registered. Reserved against mounts beside `/ui`
        (`tree/Mount.cpp`), because a mount there would be published in the
        tree and unreachable over HTTP at once - and would read as a cache
        miss rather than the collision it was. */
    constexpr const char* mediaRoutePrefix = "/media";

    /*  One request: `path` is the whole request path, the prefix included,
        and `query` all of what followed its `?`.

        CHECKED IN THIS ORDER, so a client is told the first thing it got
        wrong - and a malformed request is refused as malformed whether or not
        the hash is known, so a 404 always means "well asked, and not here":
          1. the path is the prefix, one segment, then `timbre` - or 404, no
             such route;
          2. the segment is sixty-four lower-case hex digits, as the engine
             publishes a hash and names its caches - or 400;
          3. the query is `INFO`, or `level=` and one to nine digits - or 400.
             Nine is where the parse stops needing an overflow check, not a
             limit anybody meets: an hour of audio at 48 kHz is thirteen
             levels;
          4. the records hold that hash WITH a pyramid - or 404, which the
             same request may turn into a 200 once the analyser has been by;
          5. the pyramid has that level - or 404.

        A 200 for `level=N` is that level's frames, four bytes each in the
        order hue, saturation, lightness, peak - exactly the bytes the `.tpy`
        holds for it - as `application/octet-stream`. A 200 for `INFO` is the
        header as `application/json`, keys in this order and no spaces, every
        number written without the locale:

            {"sha256":"<hash>","formatVersion":<v>,"seconds":<s>,
             "sampleRate":<Hz>,"window":2048,"hop":1024,
             "levels":[{"frames":<n>,"bytes":<4n>},...]}

        `formatVersion` is `timbre::formatVersion`, the analysis that built
        the frames - second, right after the name, because it is the half of
        what the bytes depend on that the name does not carry.

        Every 200 carries `Cache-Control: no-cache`, for the reason above;
        every refusal is one line of `text/plain` and carries `no-store`. */
    RouteReply answerTimbreRoute (const audio::MediaInfo& media,
                                  const std::string& path,
                                  const std::string& query);
}
