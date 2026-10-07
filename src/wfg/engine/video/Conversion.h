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
    A MOVIE CONVERTED TO HAP, IN THE BACKGROUND (namespace draft 37.5 WF-WK,
    37.6 F.3).

    The author's: a movie that is not HAP is a preview; adding it to the show
    converts it to HAP in the background (WF), of the whole file or only the
    part the cues use with ten seconds either side (WI), and when that is done
    the cue names the HAP file, one undoable edit (WH).

    `convertMovie` is the whole of it for one file, on the caller's thread:
    FFmpeg decodes the source's frames to raw RGBA down a pipe, Go.dot's
    encoder makes each a HAP frame on as many threads as the machine spares,
    and the writer streams them to `<target>.part`, renamed to the target
    when the last is written - so a file under the target's name is always a
    whole one. Its sound, when asked, is FFmpeg's to take out alone, to a
    24-bit WAV beside it over the same span (WJ).

    `Converter` runs them one at a time on a thread of its own, for `serve`:
    MediaAnalyser's shape, slow work handed over by a thread that never
    waits. What a finished one becomes is a COMMAND, `media.converted`,
    submitted to the engine like any other - the edit is in the log, undoable,
    and a replay makes it without converting anything.

    `media.convert <file> <scope> <format> <sound>` asks for one: scope whole
    or used, format hap or hapq. With no converter - every verb that replays
    a log - it is taken and does nothing, as a test pattern is.
*/

#include <wfg/engine/video/Hap.h>

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wfg { class CommandRegistry; class Engine; }
namespace wfg::doc { class ShowDocument; }

namespace wfg::video
{
    /*  ONE CONVERSION, in paths and seconds: nothing of the document. */
    struct ConversionRequest
    {
        std::string id;                 ///< unique tonight
        std::string sourceName;         ///< the source as the document names it, under media/
        std::string source;             ///< and where it is on this machine
        std::string targetName;         ///< the HAP file as the document will name it
        std::string target;
        double start = 0.0;             ///< seconds of the source the HAP's first frame is
        double length = -1.0;           ///< seconds after it, or below nought for to the end
        bool quality = false;           ///< Hap Q rather than Hap / Hap Alpha
        std::string soundName;          ///< the sound's file as the document will name it; empty: none
        std::string sound;

        /*  How many channels the sound came out with, told once it has: what
            its cue's route is laid out for. */
        int soundChannels = 0;
    };

    /*  HOW ONE IS GOING, for the readouts. */
    struct ConversionStatus
    {
        std::string id;
        std::string sourceName;
        std::string state;              ///< waiting, converting, done, failed, cancelled
        double progress = 0.0;          ///< 0..1
        std::string problem;
    };

    /*  The whole of one conversion, here and now. `progress` is told 0..1 as
        frames are written; `cancelled` is read between frames. False with
        `why` said; nothing is left under the target's name then. No target
        is the sound alone - a movie already HAP whose sound is wanted. */
    bool convertMovie (ConversionRequest& request, std::string& why,
                       const std::function<void (double)>& progress = {},
                       const std::atomic<bool>* cancelled = nullptr);

    /*  THE USED PART OF A SOURCE (WI): every cue naming it, from the earliest
        it plays - its start offset, or its first Range's in point (WL) - to
        the furthest - the file's end for a cue with no Range, else its last
        out point - with ten seconds either side where the file has them.
        Seconds of the source; `end` below nought is to the file's end. The
        start is what is cut from the front, and every start offset and every
        in and out point moves back by it. */
    struct UsedSpan
    {
        double start = 0.0;
        double end = -1.0;
    };

    UsedSpan usedSpanOf (const doc::ShowDocument& document, const std::string& sourceName);

    class Converter
    {
    public:
        /*  `finished` is told on the converter's thread when one is done,
            failed or cancelled - serve's submits the command. */
        using Finished = std::function<void (const ConversionRequest&, const ConversionStatus&)>;

        explicit Converter (Finished finished);
        ~Converter();

        Converter (const Converter&) = delete;
        Converter& operator= (const Converter&) = delete;

        /*  Where the show's media are: set when a show opens, read when a
            conversion is asked for. */
        void setMediaFolder (std::string folder);
        std::string mediaFolder() const;

        void enqueue (ConversionRequest request);
        void cancel (const std::string& sourceName);

        /*  Every one asked for this session, the last of each source. */
        std::vector<ConversionStatus> statuses() const;

        /*  Where FFmpeg is, as last looked for - when the converter was made,
            and at each conversion asked for. Empty when it was not found. */
        std::string ffmpegPath() const;

        void stop();

    private:
        void run();

        Finished finished;
        mutable std::mutex lock;
        std::condition_variable wake;
        std::deque<ConversionRequest> queue;
        std::vector<ConversionStatus> known;
        std::string folder;
        mutable std::string tools;
        mutable std::uint32_t lookedAt = 0;
        std::string current;
        std::atomic<bool> cancelCurrent { false };
        bool stopping = false;
        std::thread thread;
    };

    /*  `media.convert` and `media.converted`. `converter` null: the first is
        taken and does nothing; the second, the edit, is made everywhere.

        `media.converted` also makes a movie's sound a cue (WJ): one after
        every video cue that names the movie, a media cue on the sound's file,
        locked to it, routed as an imported sound is - or, for a movie that
        has one already, that one pointed at the new file. The identifiers it
        drew go on its record, which a replay re-supplies. */
    void registerConversionCommands (CommandRegistry& registry, doc::ShowDocument& document, Converter* converter);
}
