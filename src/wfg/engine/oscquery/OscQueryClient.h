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
    Asking somebody else's box what a value actually is.

    THIS IS THE HALF OF A CLOSED LOOP GO.DOT DID NOT HAVE. Everything before it
    asserts: a cue writes a node and the message goes out. PRD §3.11 wants the
    other direction - read the value back and compare - because that is what
    turns a list of network cues into a chain that can be relied on, and what
    makes a failure visible in the cue list rather than discovered by ear.

    WHY IT IS WRITTEN HERE RATHER THAN REACHED FOR. There is no HTTP client
    anywhere in this project or in either submodule: `juce_simpleweb` ships a
    WebSocket client and two HTTP SERVERS, and upstream's `client_http.hpp` is
    not in the fork. So the choice was juce::URL or a socket, and juce::URL is
    wrong for OSCQuery in two separately-measured ways:

      * IT RE-ENCODES THE QUERY STRING. OSCQuery asks with a bare key -
        `?HOST_INFO`, `?VALUE` - and juce::URL turns that into `?HOST_INFO=`,
        which a conforming server does not match. WFS-DIY hit this and
        abandoned juce::URL in its own client for the same reason.
      * IT NORMALISES THE STATUS. It was observed reporting 200 for this
        project's own server answering 204, and 204 is load-bearing here: it
        means the node is real and the attribute is absent, which is a
        different answer from 404 and from a JSON null.

    So: write the request, read the reply, parse the status line. It is short
    and it tells the truth.

    IT BLOCKS, AND IT MUST NEVER RUN ON THE TICK THREAD. `connect`, `write` and
    `waitUntilReady` are all syscalls with a deadline attached, and a device
    that has gone away takes the whole deadline to say so. PRD §4.1 is about the
    GO path and §4.2 about the audio thread, but a 50 Hz clock that stops for
    two seconds is a show that stopped. MountProbe is what owns the thread this
    runs on; nothing else may call it.

    WHAT IT IS NOT. Not a general HTTP client: GET only, no redirects, no
    keep-alive, no TLS - and TLS is not an omission but a constraint, because
    the binary links no OpenSSL and a ctest inspects it to make sure. It reads
    `Content-Length` and chunked bodies and nothing more exotic, which covers
    every OSCQuery server anybody has actually shipped.
*/

#include <wfg/engine/osc/OscValue.h>

#include <optional>
#include <string>

namespace wfg::oscquery
{
    struct HttpReply
    {
        /** The exchange completed. Says nothing about what the server thought. */
        bool ok = false;

        /*  The HTTP status, read off the wire rather than interpreted. 200
            found, 204 the node is real and this attribute is not, 400 not an
            OSCQuery attribute, 404 no such node, 405 not a GET. */
        int status = 0;

        std::string body;

        /** Why the exchange did not complete, when it did not. */
        std::string error;
    };

    class OscQueryClient
    {
    public:
        /*  One GET, blocking, with a deadline in milliseconds.

            `query` is appended after a `?` VERBATIM and is never escaped, which
            is the whole reason this exists: `VALUE` must arrive as `?VALUE`.
            Pass it empty for a plain path. */
        static HttpReply get (const std::string& host, int port, const std::string& path,
                              const std::string& query, int timeoutMs);

        /*  What a target says one of its nodes currently holds.

            `typeTag` is the node's declared OSC type from the mounted
            description, and the answer is coerced to it - so a server that
            writes `0.5` for a float and `1` for an integer produces a value
            that compares equal to what was written, rather than one that
            differs by being a double.

            nullopt when the exchange failed, when the server answered anything
            but 200, or when the reply carried no VALUE. Each is a different
            thing and none of them is "the value is zero". */
        static std::optional<osc::Value> readValue (const std::string& host, int port,
                                                    const std::string& address,
                                                    const std::string& typeTag,
                                                    int timeoutMs);

        /*  The VALUE out of an OSCQuery attribute reply, for a test or a caller
            that already has the bytes. Exposed because the parsing is the part
            worth checking against a string literal rather than against a
            socket. */
        static std::optional<osc::Value> valueFromReply (std::string_view json,
                                                         const std::string& typeTag);
    };
}
