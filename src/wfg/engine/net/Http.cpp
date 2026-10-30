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

#include <wfg/engine/net/Http.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace wfg::net
{
    namespace
    {
        std::string lower (std::string text)
        {
            std::transform (text.begin(), text.end(), text.begin(),
                            [] (unsigned char c) { return static_cast<char> (std::tolower (c)); });
            return text;
        }

        std::string trimmed (std::string_view text)
        {
            const auto first = text.find_first_not_of (" \t");
            if (first == std::string_view::npos)
                return {};
            return std::string (text.substr (first, text.find_last_not_of (" \t") - first + 1));
        }
    }

    std::string base64 (std::string_view bytes)
    {
        static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        std::size_t at = 0;

        for (; at + 2 < bytes.size(); at += 3)
        {
            const auto n = (static_cast<unsigned> (static_cast<unsigned char> (bytes[at])) << 16)
                         | (static_cast<unsigned> (static_cast<unsigned char> (bytes[at + 1])) << 8)
                         | static_cast<unsigned> (static_cast<unsigned char> (bytes[at + 2]));
            out += alphabet[(n >> 18) & 63u];
            out += alphabet[(n >> 12) & 63u];
            out += alphabet[(n >> 6) & 63u];
            out += alphabet[n & 63u];
        }

        if (const auto rest = bytes.size() - at; rest > 0)
        {
            auto n = static_cast<unsigned> (static_cast<unsigned char> (bytes[at])) << 16;
            if (rest == 2)
                n |= static_cast<unsigned> (static_cast<unsigned char> (bytes[at + 1])) << 8;
            out += alphabet[(n >> 18) & 63u];
            out += alphabet[(n >> 12) & 63u];
            out += rest == 2 ? alphabet[(n >> 6) & 63u] : '=';
            out += '=';
        }

        return out;
    }

    std::string requestOf (const std::string& method, const std::string& host, const std::string& path,
                           const std::string& user, const std::string& password,
                           const std::string& accept, const std::string& body)
    {
        std::string out = method + " " + path + " HTTP/1.1\r\n";
        out += "Host: " + host + "\r\n";

        if (! user.empty())
            out += "Authorization: Basic " + base64 (user + ":" + password) + "\r\n";

        out += "Accept: " + (accept.empty() ? std::string ("application/json") : accept) + "\r\n";
        out += "Connection: keep-alive\r\n";

        if (! body.empty() || method == "PUT" || method == "POST")
        {
            out += "Content-Type: application/json\r\n";
            out += "Content-Length: " + std::to_string (body.size()) + "\r\n";
        }

        out += "\r\n";
        out += body;
        return out;
    }

    std::optional<ResponseHead> takeHead (std::string& buffer, std::string& problem)
    {
        const auto end = buffer.find ("\r\n\r\n");

        if (end == std::string::npos)
            return std::nullopt;

        const std::string head = buffer.substr (0, end);
        buffer.erase (0, end + 4);

        ResponseHead out;
        std::size_t lineStart = 0;
        auto first = true;

        while (lineStart <= head.size())
        {
            auto lineEnd = head.find ("\r\n", lineStart);
            if (lineEnd == std::string::npos)
                lineEnd = head.size();

            const std::string_view line (head.data() + lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 2;

            if (first)
            {
                first = false;

                //  "HTTP/1.1 200 OK"
                if (line.substr (0, 5) != "HTTP/" || line.size() < 12)
                {
                    problem = "not an HTTP response";
                    return std::nullopt;
                }

                out.status = std::atoi (std::string (line.substr (9, 3)).c_str());
                out.reason = line.size() > 13 ? std::string (line.substr (13)) : std::string {};
                continue;
            }

            if (const auto colon = line.find (':'); colon != std::string_view::npos)
                out.headers[lower (trimmed (line.substr (0, colon)))] = trimmed (line.substr (colon + 1));

            if (lineEnd == head.size())
                break;
        }

        if (const auto found = out.headers.find ("transfer-encoding"); found != out.headers.end())
            out.chunked = lower (found->second).find ("chunked") != std::string::npos;

        if (const auto found = out.headers.find ("content-length"); found != out.headers.end())
            out.contentLength = std::atoll (found->second.c_str());

        if (const auto found = out.headers.find ("connection"); found != out.headers.end())
            out.closes = lower (found->second) == "close";

        return out;
    }

    std::string ChunkDecoder::feed (std::string_view bytes)
    {
        std::string out;
        pending.append (bytes);

        while (! finished && problem.empty())
        {
            if (inData)
            {
                //  The chunk's bytes, then its CRLF.
                const auto take = static_cast<std::size_t> (std::min<long long> (left, static_cast<long long> (pending.size())));
                const auto payload = left > 2 ? std::min (take, static_cast<std::size_t> (left - 2)) : 0u;
                out.append (pending, 0, payload);
                pending.erase (0, take);
                left -= static_cast<long long> (take);

                if (left > 0)
                    return out;

                inData = false;
                continue;
            }

            const auto lineEnd = pending.find ("\r\n");
            if (lineEnd == std::string::npos)
                return out;

            const auto sizeText = pending.substr (0, lineEnd);
            pending.erase (0, lineEnd + 2);

            char* stop = nullptr;
            const auto size = std::strtoll (sizeText.c_str(), &stop, 16);

            if (stop == sizeText.c_str() || size < 0)
            {
                problem = "a chunk size that is not one: " + sizeText;
                return out;
            }

            if (size == 0)
            {
                finished = true;
                return out;
            }

            left = size + 2;
            inData = true;
        }

        return out;
    }

    std::vector<ServerEvent> EventParser::feed (std::string_view text)
    {
        std::vector<ServerEvent> out;

        const auto endLine = [this, &out]
        {
            if (line.empty())
            {
                if (any)
                    out.push_back (current);
                current = {};
                any = false;
                sawData = false;
                return;
            }

            if (line[0] != ':')
            {
                const auto colon = line.find (':');
                const auto field = line.substr (0, colon);
                auto value = colon == std::string::npos ? std::string {} : line.substr (colon + 1);
                if (! value.empty() && value[0] == ' ')
                    value.erase (0, 1);

                if (field == "event")
                {
                    current.name = value;
                    any = true;
                }
                else if (field == "data")
                {
                    if (sawData)
                        current.data += '\n';
                    current.data += value;
                    sawData = true;
                    any = true;
                }
            }

            line.clear();
        };

        for (const char c : text)
        {
            if (c == '\n' && lastWasCr)
            {
                lastWasCr = false;
                continue;
            }

            lastWasCr = c == '\r';

            if (c == '\r' || c == '\n')
                endLine();
            else
                line.push_back (c);
        }

        return out;
    }
}
