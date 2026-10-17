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

#include <wfg/client/model/Updates.h>
#include <wfg/engine/json/JsonValue.h>

#include <tuple>

namespace wfg::client::model::updates
{
    namespace
    {
        /*  ONE NUMBER OF A VERSION: digits only, no sign, no space, and few
            enough of them that no int overflows - a million minor versions is
            not a release anybody will cut. Read by hand, not by a library that
            reads the locale or a leading plus. */
        std::optional<int> readPart (std::string_view text)
        {
            if (text.empty() || text.size() > 6)
                return std::nullopt;

            int value = 0;

            for (const auto c : text)
            {
                if (c < '0' || c > '9')
                    return std::nullopt;

                value = value * 10 + (c - '0');
            }

            return value;
        }

        std::optional<std::int64_t> readSeconds (std::string_view text)
        {
            if (text.empty() || text.size() > 18)
                return std::nullopt;

            std::int64_t value = 0;

            for (const auto c : text)
            {
                if (c < '0' || c > '9')
                    return std::nullopt;

                value = value * 10 + (c - '0');
            }

            return value;
        }

        bool startsWith (std::string_view text, std::string_view head)
        {
            return text.size() >= head.size() && text.substr (0, head.size()) == head;
        }

        /*  A FLAG GITHUB SETS, read as set only when it is `true`. Absent or not a
            boolean is not set: a reply without the field is the ordinary case. */
        bool flagged (const json::Value& reply, std::string_view key)
        {
            const auto* found = reply.find (key);
            return found != nullptr && found->isBool() && found->asBool();
        }
    }

    std::optional<Version> readVersion (std::string_view text)
    {
        if (! text.empty() && text.front() == 'v')
            text.remove_prefix (1);

        const auto first = text.find ('.');

        if (first == std::string_view::npos)
            return std::nullopt;

        const auto second = text.find ('.', first + 1);

        if (second == std::string_view::npos)
            return std::nullopt;

        const auto major = readPart (text.substr (0, first));
        const auto minor = readPart (text.substr (first + 1, second - first - 1));
        const auto patch = readPart (text.substr (second + 1));

        if (! major || ! minor || ! patch)
            return std::nullopt;

        return Version { *major, *minor, *patch };
    }

    std::string versionText (const Version& version)
    {
        return std::to_string (version.major) + "." + std::to_string (version.minor) + "."
                 + std::to_string (version.patch);
    }

    bool isNewer (const Version& candidate, const Version& running)
    {
        return std::tie (candidate.major, candidate.minor, candidate.patch)
                 > std::tie (running.major, running.minor, running.patch);
    }

    std::optional<Release> readRelease (std::string_view reply)
    {
        if (reply.size() > largestReply)
            return std::nullopt;

        const auto parsed = json::parse (reply);

        if (! parsed.ok() || ! parsed.value->isObject())
            return std::nullopt;

        const auto& body = *parsed.value;

        /*  `latest` never names either, by GitHub's own account; a reply that
            says otherwise is not the one this asked for. */
        if (flagged (body, "draft") || flagged (body, "prerelease"))
            return std::nullopt;

        const auto* tag = body.find ("tag_name");
        const auto* page = body.find ("html_url");

        if (tag == nullptr || ! tag->isString() || page == nullptr || ! page->isString())
            return std::nullopt;

        //  The tag is `v` and three numbers: the release workflow cuts no other (ASX).
        if (! startsWith (tag->asString(), "v"))
            return std::nullopt;

        const auto version = readVersion (tag->asString());

        if (! version || ! startsWith (page->asString(), ownReleases))
            return std::nullopt;

        return Release { *version, page->asString() };
    }

    Answer answerFor (int status, std::string_view body)
    {
        Answer answer;

        //  None published yet is an answer too: GitHub says so with a 404.
        if (status == 404)
        {
            answer.reached = true;
            return answer;
        }

        if (status != 200 || body.size() > largestReply)
            return answer;

        answer.reached = true;
        answer.release = readRelease (body);
        return answer;
    }

    Answer answerFromCurl (std::string_view output)
    {
        const auto cut = output.rfind ('\n');

        if (cut == std::string_view::npos)
            return {};

        const auto code = output.substr (cut + 1);

        if (code.size() != 3)
            return {};

        const auto status = readPart (code);

        //  `000`: curl never had an answer to give a status for.
        if (! status || *status == 0)
            return {};

        return answerFor (*status, output.substr (0, cut));
    }

    Answer answerFromWget (int exitCode, std::string_view output)
    {
        return answerFor (exitCode == 0 ? 200 : -1, output);
    }

    Memory readMemory (std::string_view text)
    {
        Memory memory;

        while (! text.empty())
        {
            const auto end = text.find ('\n');
            auto line = text.substr (0, end);
            text = end == std::string_view::npos ? std::string_view {} : text.substr (end + 1);

            if (! line.empty() && line.back() == '\r')
                line.remove_suffix (1);

            const auto space = line.find (' ');

            if (space == std::string_view::npos)
                continue;

            const auto key = line.substr (0, space);
            const auto value = line.substr (space + 1);

            if (key == "ask")
                memory.askAtLaunch = value != "off";
            else if (key == "asked")
                memory.lastAsked = readSeconds (value).value_or (0);
            else if (key == "found" && readVersion (value))
                memory.found = std::string (value);
            else if (key == "page" && startsWith (value, ownReleases))
                memory.foundPage = std::string (value);
            else if (key == "dismissed" && readVersion (value))
                memory.dismissed = std::string (value);
        }

        //  A version found with no page to send anybody to is not offered.
        if (memory.foundPage.empty())
            memory.found.clear();

        return memory;
    }

    std::string writeMemory (const Memory& memory)
    {
        std::string out;
        out += "ask " + std::string (memory.askAtLaunch ? "on" : "off") + "\n";
        out += "asked " + std::to_string (memory.lastAsked) + "\n";

        if (! memory.found.empty() && ! memory.foundPage.empty())
        {
            out += "found " + memory.found + "\n";
            out += "page " + memory.foundPage + "\n";
        }

        if (! memory.dismissed.empty())
            out += "dismissed " + memory.dismissed + "\n";

        return out;
    }

    bool dueToAsk (const Memory& memory, std::int64_t now)
    {
        if (! memory.askAtLaunch)
            return false;

        return now < memory.lastAsked || now - memory.lastAsked >= secondsBetweenAsks;
    }

    void remember (Memory& memory, std::int64_t now, const std::optional<Release>& release)
    {
        memory.lastAsked = now;

        if (release)
        {
            memory.found = versionText (release->version);
            memory.foundPage = release->page;
        }
        else
        {
            memory.found.clear();
            memory.foundPage.clear();
        }
    }

    std::optional<Version> markFor (const Memory& memory, const Version& running)
    {
        const auto found = readVersion (memory.found);

        if (! found || memory.foundPage.empty() || ! isNewer (*found, running))
            return std::nullopt;

        if (memory.dismissed == memory.found)
            return std::nullopt;

        return found;
    }

    std::string checkedWords (bool reached, const std::optional<Release>& release, const Version& running)
    {
        if (! reached)
            return "Could not reach GitHub to look for a newer Go.dot.";

        if (release && isNewer (release->version, running))
            return "Go.dot " + versionText (release->version) + " is out - the Help menu opens its page.";

        return "No newer Go.dot than this one, " + versionText (running) + ".";
    }

    std::string markWords (const Version& version)
    {
        return "Go.dot " + versionText (version) + " is out";
    }
}
