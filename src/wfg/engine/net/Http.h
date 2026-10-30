// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/*  HTTP/1.1, AS MUCH AS A BASE STATION NEEDS (namespace draft §124, BET).

    A Sennheiser Spectera answers REST and JSON over HTTPS and tells its
    changes as Server-Sent Events on one long GET. These are the pieces of that
    with no socket in them - a request's text, a response's head, a chunked
    body, an event stream - so a test holds them with bytes alone; the TLS
    stream and the worker that owns it are elsewhere (`TlsStream`, `SscV2`).
    Not a general client: no redirects, no cookies, no compression asked for. */

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::net
{
    /*  A request whole: the line, Host, Authorization (Basic, from the user
        and password, base64'd) when a user is given, Accept, and the body as
        JSON with its length; keep-alive. */
    std::string requestOf (const std::string& method, const std::string& host, const std::string& path,
                           const std::string& user, const std::string& password,
                           const std::string& accept, const std::string& body);

    std::string base64 (std::string_view bytes);

    /*  A response's head, once its blank line has come: the status, the
        headers by lower-cased name, whether the body is chunked, and its
        length when it says one (-1 when it does not). */
    struct ResponseHead
    {
        int status = 0;
        std::string reason;
        std::map<std::string, std::string> headers;
        bool chunked = false;
        long long contentLength = -1;
        bool closes = false;
    };

    /*  Takes the head off the front of `buffer` once it is all there: nothing
        while the blank line has not come, and the head with `buffer` left
        holding what follows it. A head that cannot be one says so in
        `problem`. */
    std::optional<ResponseHead> takeHead (std::string& buffer, std::string& problem);

    /*  A CHUNKED BODY, however the bytes come: the payload out as it arrives,
        `finished` once the last chunk has, `problem` for a size that is not
        one. */
    struct ChunkDecoder
    {
        std::string pending;
        long long left = 0;          // of the chunk being read, and its CRLF
        bool inData = false;
        bool finished = false;
        std::string problem;

        std::string feed (std::string_view bytes);
    };

    /*  SERVER-SENT EVENTS (the EventSource format): the text cut into events at
        a blank line, each its `event` name - "message" when none is said - and
        its `data` lines joined by new lines; a comment line (":") and an
        unknown field are nothing. CR LF, CR and LF all end a line. */
    struct ServerEvent
    {
        std::string name = "message";
        std::string data;
    };

    struct EventParser
    {
        std::string line;
        ServerEvent current;
        bool any = false;
        bool sawData = false;
        bool lastWasCr = false;

        std::vector<ServerEvent> feed (std::string_view text);
    };
}
