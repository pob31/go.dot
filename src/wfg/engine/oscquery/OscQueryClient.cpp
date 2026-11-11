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

#include <wfg/engine/oscquery/OscQueryClient.h>

#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/net/Http.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>

namespace wfg::oscquery
{
    namespace
    {
        /*  How long is left of a deadline, never below zero and never above
            what was asked for. Every socket call takes a timeout of its own, so
            a request with a 2000 ms budget that spent 1800 ms connecting has
            200 ms left to read in - rather than 2000 more. */
        int remaining (std::chrono::steady_clock::time_point deadline)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds> (
                                deadline - std::chrono::steady_clock::now()).count();

            return static_cast<int> (std::clamp<std::int64_t> (left, 0, 600000));
        }

        /*  WHERE A REPLY ENDS, when the server says so itself (namespace draft
            §129, BFX; RFC 9112 §6.3):
            at its Content-Length, or at its last chunk. The request says
            `Connection: close`, and a server that honours it ends the reply
            by closing; one that answers with a length and then keeps the
            connection open is at fault under HTTP/1.1, and exists - every
            request to it used to wait out its whole timeout and fail (Julien
            Bayle's retest of the OSCQuery Automation Plugin, item 2.9). A
            reply with neither still ends at the close, as before; so does one
            that is not HTTP, which the reading after the loop names.
            Incremental: each read is looked at once.

            A CHUNKED BODY IS PUT BACK TOGETHER HERE, and nowhere else (§130,
            BFZ). Rare against an OSCQuery server and not hypothetical: the
            framing is the server's choice, and a reply with its size markers
            still in it would parse as broken JSON and be reported as a device
            that answered nonsense. One decoder decides both where the reply
            ends and what its body is - a second one, reading the header its
            own way, once took `Transfer-Encoding:chunked` without its space
            for a plain body, and wrapped round on a size near 2^64. */
        struct ReplyEnd
        {
            std::size_t searchedTo = 0;    ///< where the blank line was last looked for
            bool headRead = false;
            bool untilClose = false;
            bool chunked = false;
            long long length = -1;
            std::size_t bodyStart = 0;
            std::size_t fed = 0;
            net::ChunkDecoder chunks;
            std::string dechunked;         ///< a chunked reply's body, so far

            bool reached (const std::string& response)
            {
                if (untilClose)
                    return false;

                if (! headRead)
                {
                    const auto blank = response.find ("\r\n\r\n", searchedTo);

                    if (blank == std::string::npos)
                    {
                        searchedTo = response.size() > 3 ? response.size() - 3 : 0;
                        return false;
                    }

                    headRead = true;
                    bodyStart = blank + 4;
                    fed = bodyStart;

                    auto headText = response.substr (0, bodyStart);
                    std::string problem;
                    const auto head = net::takeHead (headText, problem);

                    //  Not HTTP, or an interim 1xx before the real answer: the close is the end.
                    if (! head.has_value() || head->status < 200)
                    {
                        untilClose = true;
                        return false;
                    }

                    //  These never carry a body, whatever else they say.
                    if (head->status == 204 || head->status == 304)
                        return true;

                    chunked = head->chunked;
                    length = chunked ? -1 : head->contentLength;

                    if (! chunked && length < 0)
                    {
                        untilClose = true;
                        return false;
                    }
                }

                if (chunked)
                {
                    dechunked += chunks.feed (std::string_view (response).substr (fed));
                    fed = response.size();
                    return chunks.finished || ! chunks.problem.empty();
                }

                return static_cast<long long> (response.size() - bodyStart) >= length;
            }
        };
    }

    //==============================================================================
    HttpReply OscQueryClient::get (const std::string& host, int port, const std::string& path,
                                   const std::string& query, int timeoutMs)
    {
        HttpReply reply;

        if (host.empty() || port <= 0 || port > 65535)
        {
            reply.error = "no usable address";
            return reply;
        }

        const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds (std::max (1, timeoutMs));

        juce::StreamingSocket socket;

        if (! socket.connect (juce::String (host), port, remaining (deadline)))
        {
            reply.error = "could not connect to " + host + ":" + std::to_string (port);
            return reply;
        }

        /*  THE QUERY GOES ON VERBATIM. OSCQuery asks with a bare key and a
            client that helpfully turned `?VALUE` into `?VALUE=` would be
            answered 400 by a conforming server, for ever, with nothing in the
            log to say why. */
        const auto target = query.empty() ? path : path + "?" + query;

        const std::string request = "GET " + target + " HTTP/1.1\r\n"
                                    "Host: " + host + ":" + std::to_string (port) + "\r\n"
                                    "User-Agent: Go.dot\r\n"
                                    "Connection: close\r\n"
                                    "\r\n";

        if (socket.write (request.data(), static_cast<int> (request.size()))
              != static_cast<int> (request.size()))
        {
            reply.error = "the request could not be written";
            return reply;
        }

        std::string response;
        char buffer[4096];
        ReplyEnd end;

        for (;;)
        {
            const auto left = remaining (deadline);

            if (left <= 0)
            {
                reply.error = "the target did not answer in time";
                return reply;
            }

            const auto ready = socket.waitUntilReady (true, left);

            if (ready < 0)
            {
                reply.error = "the connection broke while reading";
                return reply;
            }

            if (ready == 0)
            {
                reply.error = "the target did not answer in time";
                return reply;
            }

            const auto read = socket.read (buffer, static_cast<int> (sizeof (buffer)), false);

            /*  Zero is the far end closing, which with `Connection: close` is
                how a complete reply ends. It is the ordinary exit from this
                loop and not a failure; the other is the end the reply's own
                framing gives (ReplyEnd, above). */
            if (read <= 0)
                break;

            response.append (buffer, static_cast<std::size_t> (read));

            /*  A guard rather than a policy: an OSCQuery description of a real
                rig is a couple of megabytes, and something answering an endless
                stream is not a device this is going to make sense of. */
            if (response.size() > 16u * 1024u * 1024u)
            {
                reply.error = "the target sent more than this is willing to read";
                return reply;
            }

            if (end.reached (response))
                break;
        }

        socket.close();

        //  "HTTP/1.1 204 No Content" -> 204
        const auto firstSpace = response.find (' ');

        if (firstSpace == std::string::npos || response.rfind ("HTTP/", 0) != 0)
        {
            reply.error = "the target did not answer with HTTP";
            return reply;
        }

        reply.status = std::atoi (response.c_str() + firstSpace + 1);

        const auto headerEnd = response.find ("\r\n\r\n");

        if (headerEnd == std::string::npos)
        {
            reply.error = "the reply had no end of headers";
            return reply;
        }

        auto body = response.substr (headerEnd + 4);

        if (end.chunked)
        {
            //  Fixed text: the decoder's own quotes the server's bytes, as many as there were.
            if (! end.chunks.problem.empty())
            {
                reply.error = "the reply's chunked body was broken";
                return reply;
            }

            body = std::move (end.dechunked);
        }
        else if (end.length >= 0 && body.size() > static_cast<std::size_t> (end.length))
        {
            body.resize (static_cast<std::size_t> (end.length));      // what the server said is the body, and no more
        }

        reply.body = std::move (body);
        reply.ok = true;
        return reply;
    }

    //==============================================================================
    std::optional<osc::Values> OscQueryClient::valueFromReply (std::string_view json,
                                                               const std::string& typeTags)
    {
        const auto parsed = wfg::json::parse (json);

        if (! parsed.value.has_value())
            return std::nullopt;

        const auto* values = parsed.value->find ("VALUE");

        if (values == nullptr || ! values->isArray() || values->asArray().empty())
            return std::nullopt;

        /*  EVERY ELEMENT, each against its own tag (namespace draft §45): a
            node of three arguments answers three, and a verify of
            `/adm/obj/1/xyz` compares all of them. One that cannot be read
            refuses the whole answer - two values of three are not what the
            device holds. */
        osc::Values read;

        for (std::size_t i = 0; i < values->asArray().size(); ++i)
        {
            const auto& element = values->asArray()[i];

            /*  JSON HAS THREE SCALAR TYPES AND OSC HAS TEN, so the node's
                declared tag is what decides. Without it a `1` in a reply is an
                integer, a `1.0` is a double, and neither compares equal to the
                float32 that was written - so a verified cue would time out
                against a device that was doing exactly what it was told. */
            osc::Value asRead;

            if (element.isNumber())
                asRead = osc::Value::float64 (element.asNumber());
            else if (element.isString())
                asRead = osc::Value::string (element.asString());
            else if (element.isBool())
                asRead = osc::Value::boolean (element.asBool());
            else
                return std::nullopt;

            if (i < typeTags.size())
            {
                auto coerced = CommandRegistry::coerceToTag (typeTags[i], asRead);

                if (! coerced.has_value())
                    return std::nullopt;

                asRead = std::move (*coerced);
            }

            read.push_back (std::move (asRead));
        }

        return read;
    }

    std::optional<osc::Values> OscQueryClient::readValue (const std::string& host, int port,
                                                          const std::string& address,
                                                          const std::string& typeTag,
                                                          int timeoutMs)
    {
        const auto reply = get (host, port, address, "VALUE", timeoutMs);

        /*  ONLY 200 IS AN ANSWER. A 204 means the node is real and has no value
            yet, 404 that the target does not have it at all, 400 that it did not
            understand the question - and each of those is a different thing to
            tell somebody, none of which is a value. Collapsing them here would
            turn "the device does not have that node" into "the device disagrees
            with you", which sends the wrong person to look. */
        if (! reply.ok || reply.status != 200)
            return std::nullopt;

        return valueFromReply (reply.body, typeTag);
    }
}
