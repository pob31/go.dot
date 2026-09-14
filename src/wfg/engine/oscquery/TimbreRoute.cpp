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

#include <wfg/engine/oscquery/TimbreRoute.h>

#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/audio/Timbre.h>
#include <wfg/engine/osc/OscValue.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::oscquery
{
    namespace
    {
        constexpr const char* textMime = "text/plain";
        constexpr const char* jsonMime = "application/json";
        constexpr const char* bytesMime = "application/octet-stream";

        /*  KEPT, BUT ASKED AGAIN BEFORE IT IS REUSED - and not a year and
            `immutable`, though the URL looks as if it could bear it. A pyramid
            is addressed by the sha256 of the audio, and its bytes are that
            audio run through this build's analysis: `timbre::formatVersion`
            moves with every change to the analysis, a ramp stop moved
            included, and the `.tpy` is then rebuilt under the same name. So
            one URL can answer two different ways, one build apart, and an
            immutable copy would show the old colours to the person who had
            just changed them. There is nothing to revalidate with - a
            `RouteHandler` sees no request headers, so no ETag and no 304 -
            and asking again is fetching again; a page keeps what it fetched
            in memory for its own life, so that is a level per bar per page
            load. Immutable can come back with a URL that carries the version
            (TimbreRoute.h). */
        constexpr const char* askedAgain = "no-cache";

        /*  AND NOT KEPT AT ALL, for every refusal. A 404 for a hash the
            analyser has not reached yet is true for a minute; stored, it would
            keep a bar grey until the browser forgot it. */
        constexpr const char* neverKept = "no-store";

        /*  What follows the hash. */
        constexpr std::string_view resourceTail { "/timbre" };

        /*  Hex digits in a sha256, and the most digits a level may be asked
            with - enough for any pyramid there will ever be, few enough that
            the sum below cannot overflow. */
        constexpr std::size_t hashLength = 64;
        constexpr std::size_t mostLevelDigits = 9;

        /*  Four bytes a frame, as the file stores it. */
        constexpr std::size_t bytesPerFrame = 4;

        //======================================================================
        RouteReply refused (int status, const std::string& reason)
        {
            RouteReply reply;
            reply.status = status;
            reply.contentType = textMime;
            reply.body = reason + "\n";
            reply.headers.emplace_back ("Cache-Control", neverKept);
            return reply;
        }

        RouteReply answered (const char* contentType, std::string body)
        {
            RouteReply reply;
            reply.status = 200;
            reply.contentType = contentType;
            reply.body = std::move (body);
            reply.headers.emplace_back ("Cache-Control", askedAgain);
            return reply;
        }

        /*  THE HASH, OR NOTHING: the single segment between the prefix and
            `/timbre`, when the path has exactly that shape, and an empty view
            for any other - the prefix alone, a trailing slash, a segment too
            many or too few, a last segment that is not `timbre`. A segment is
            never empty, so an empty answer cannot be mistaken for one. */
        std::string_view segmentOf (std::string_view path)
        {
            const std::string_view prefix { mediaRoutePrefix };

            //  The prefix, a slash, at least one character, then the tail.
            if (path.size() < prefix.size() + 2 + resourceTail.size()
                  || path.substr (0, prefix.size()) != prefix
                  || path[prefix.size()] != '/'
                  || path.substr (path.size() - resourceTail.size()) != resourceTail)
                return {};

            const auto segment = path.substr (prefix.size() + 1,
                                              path.size() - prefix.size() - 1 - resourceTail.size());

            if (segment.find ('/') != std::string_view::npos)
                return {};

            return segment;
        }

        /*  LOWER-CASE ONLY, as the engine publishes a hash and names its cache
            files. The lookup below is an exact comparison with what
            `/godot/cue/<id>/hash` said, and a client that has a hash has it
            from there; folding the case here would be a second spelling of a
            key the engine only ever writes one way. */
        bool isContentHash (std::string_view text)
        {
            if (text.size() != hashLength)
                return false;

            for (const auto c : text)
                if (! ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    return false;

            return true;
        }

        /*  What the query asks for: the header, a level, or nothing this
            route understands. */
        struct Question
        {
            bool understood = false;
            bool info = false;
            std::size_t level = 0;
        };

        /*  `INFO`, or `level=` and one to nine ASCII digits, and nothing else -
            no sign, no space, no second parameter. This route is the only
            place on the port where a `=` is accepted at all (an OSCQuery
            attribute query is a bare key, and the tree refuses the rest), so
            it accepts exactly the one spelling it documents and refuses the
            others rather than half-reading them. */
        Question questionOf (std::string_view query)
        {
            Question question;

            if (query == "INFO")
            {
                question.understood = true;
                question.info = true;
                return question;
            }

            constexpr std::string_view levelKey { "level=" };

            if (query.size() <= levelKey.size()
                  || query.size() > levelKey.size() + mostLevelDigits
                  || query.substr (0, levelKey.size()) != levelKey)
                return question;

            for (const auto c : query.substr (levelKey.size()))
            {
                if (c < '0' || c > '9')
                    return Question {};

                question.level = question.level * 10u + static_cast<std::size_t> (c - '0');
            }

            question.understood = true;
            return question;
        }

        /*  The pyramid of the first record carrying this hash, or nullptr.
            Two files with the same bytes have the same hash and the same
            pyramid, so which of them answers does not matter. A record with a
            hash and no pyramid is passed over: the analyser publishes neither
            without the other, and a hash this route could not serve would be
            one it had promised a client for nothing. */
        const audio::TimbrePyramid* pyramidFor (const audio::MediaRecords& records,
                                                std::string_view hash)
        {
            for (const auto& entry : records)
            {
                const auto& record = entry.second;

                if (record.pyramid != nullptr && record.contentHash == hash)
                    return record.pyramid.get();
            }

            return nullptr;
        }

        /*  THE HEADER, BY HAND. Every value is a number or a hash of hex
            digits, so nothing needs escaping, and the order is the documented
            one - which a writer that sorted its keys would not keep. The
            seconds go through the shortest round-trip formatter, the same as
            every other number the engine publishes, so a French locale cannot
            turn 3.5 into "3,5" and the JSON into something no client parses.

            `formatVersion` straight after the hash: the two together are what
            the bytes depend on, and the URL carries only the first. It is this
            build's constant rather than anything read from the pyramid,
            because a pyramid in the records is one this build computed or
            accepted - `timbre::read` refuses a `.tpy` of any other version,
            and the analyser builds it again. */
        std::string infoOf (std::string_view hash, const audio::TimbrePyramid& pyramid)
        {
            const auto seconds = pyramid.sampleRate > 0
                                   ? static_cast<double> (pyramid.samples)
                                       / static_cast<double> (pyramid.sampleRate)
                                   : 0.0;

            std::string json = "{\"sha256\":\"";
            json += hash;
            json += "\",\"formatVersion\":" + std::to_string (audio::timbre::formatVersion);
            json += ",\"seconds\":" + osc::formatDouble (seconds);
            json += ",\"sampleRate\":" + std::to_string (pyramid.sampleRate);
            json += ",\"window\":" + std::to_string (audio::timbre::windowSize);
            json += ",\"hop\":" + std::to_string (audio::timbre::hopSize);
            json += ",\"levels\":[";

            for (std::size_t level = 0; level < pyramid.levels.size(); ++level)
            {
                const auto frames = pyramid.levels[level].size();

                if (level > 0)
                    json += ",";

                json += "{\"frames\":" + std::to_string (frames)
                        + ",\"bytes\":" + std::to_string (frames * bytesPerFrame) + "}";
            }

            json += "]}";
            return json;
        }

        /*  ONE LEVEL, AS THE FILE HOLDS IT: hue, saturation, lightness, peak,
            frame after frame (`timbre::write`). Built from the frames in
            memory rather than read from the `.tpy`, which is the analyser's
            file and on this thread would be a disk read - and the same bytes
            either way, which the tests check against `write` itself. */
        std::string bytesOf (const std::vector<audio::timbre::Frame>& frames)
        {
            std::string bytes;
            bytes.reserve (frames.size() * bytesPerFrame);

            for (const auto& frame : frames)
            {
                bytes.push_back (static_cast<char> (frame.hue));
                bytes.push_back (static_cast<char> (frame.saturation));
                bytes.push_back (static_cast<char> (frame.lightness));
                bytes.push_back (static_cast<char> (frame.peak));
            }

            return bytes;
        }
    }

    //==========================================================================
    RouteReply answerTimbreRoute (const audio::MediaInfo& media,
                                  const std::string& path,
                                  const std::string& query)
    {
        const auto hash = segmentOf (path);

        /*  No request text is echoed into a refusal but a hash this has
            already found to be hex: a path can carry bytes a terminal would
            act on, and a reason is one line a person reads. */
        if (hash.empty())
            return refused (404, "no such route: a timbre is at " + std::string (mediaRoutePrefix)
                                   + "/<sha256>/timbre");

        if (! isContentHash (hash))
            return refused (400, "not a content hash: a media hash is sixty-four "
                                 "lower-case hex digits");

        const auto question = questionOf (query);

        if (! question.understood)
            return refused (400, "a timbre is asked for with ?INFO or ?level=<n>");

        /*  ONE SNAPSHOT, held to the end of the request: a pointer copy under
            the one short lock `MediaInfo` has, after which nothing here can be
            changed by anybody - a record the analyser publishes meanwhile is a
            new map, and this one keeps its pyramid alive until the reply is
            built. */
        const auto records = media.snapshot();
        const auto* const pyramid = records != nullptr ? pyramidFor (*records, hash) : nullptr;

        if (pyramid == nullptr)
            return refused (404, "no timbre has been analysed for " + std::string (hash));

        if (question.info)
            return answered (jsonMime, infoOf (hash, *pyramid));

        const auto levels = pyramid->levels.size();

        if (question.level >= levels)
            return refused (404, "no level " + std::to_string (question.level)
                                   + " in this pyramid, which has " + std::to_string (levels)
                                   + (levels == 1 ? " level" : " levels"));

        return answered (bytesMime, bytesOf (pyramid->levels[question.level]));
    }
}
