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

#include <wfg/client/ui/UpdateCheck.h>

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <thread>
#include <utility>

namespace wfg::client::ui
{
   #if JUCE_LINUX
    namespace
    {
        /*  A PROGRAM'S OUTPUT, read to the cap and a little past it, so a reply
            that would not stop is stopped (ASU); `answerFor` calls one past the
            cap no answer. */
        std::string outputOf (juce::ChildProcess& child)
        {
            std::string got;
            char buffer[8192];

            for (;;)
            {
                const auto read = child.readProcessOutput (buffer, static_cast<int> (sizeof (buffer)));

                if (read <= 0)
                    break;

                got.append (buffer, static_cast<std::size_t> (read));

                if (got.size() > model::updates::largestReply + 64)
                {
                    child.kill();
                    break;
                }
            }

            child.waitForProcessToFinish (2000);
            return got;
        }

        /*  LINUX ASKS THROUGH THE SYSTEM'S OWN PROGRAM (ASY). Go.dot speaks no TLS
            - `deps.no-openssl` asserts no OpenSSL is in the binary, and libcurl
            would bring it - and JUCE's Linux web stream without libcurl speaks
            plain http alone. So curl is asked, and wget when curl is not there:
            a program that is not installed starts and prints nothing, which is
            how its absence is known. Neither: no answer, said as GitHub not
            reached. */
        model::updates::Answer askThroughATool (const std::string& url, const std::string& running)
        {
            const auto agent = "User-Agent: Go.dot/" + juce::String::fromUTF8 (running.c_str());
            const juce::String accept ("Accept: application/vnd.github+json");
            const auto address = juce::String::fromUTF8 (url.c_str());

            {
                juce::ChildProcess curl;

                if (curl.start (juce::StringArray { "curl", "--silent", "--location", "--max-time", "15",
                                                    "--header", agent, "--header", accept,
                                                    "--write-out", "\n%{http_code}", address },
                                juce::ChildProcess::wantStdOut))
                    if (const auto said = outputOf (curl); ! said.empty())
                        return model::updates::answerFromCurl (said);
            }

            juce::ChildProcess wget;

            if (! wget.start (juce::StringArray { "wget", "--quiet", "--output-document=-", "--timeout=15", "--tries=1",
                                                  "--header=" + agent, "--header=" + accept, address },
                              juce::ChildProcess::wantStdOut))
                return {};

            const auto said = outputOf (wget);
            return model::updates::answerFromWget (static_cast<int> (wget.getExitCode()), said);
        }
    }
   #endif

    UpdateCheck::UpdateCheck (std::string urlToAsk, std::string versionRunning)
        : url (std::move (urlToAsk)), version (std::move (versionRunning))
    {
    }

    UpdateCheck::~UpdateCheck()
    {
        //  Never joined (the header says why): the worker reads this and gives up.
        alive->store (false);
    }

    void UpdateCheck::ask (Done done)
    {
        std::thread ([token = alive, urlToAsk = url, running = version, done = std::move (done)]
        {
            auto answer = askNow (urlToAsk, running);

            if (! token->load())
                return;

            juce::MessageManager::callAsync ([token, answer = std::move (answer), done]
            {
                if (token->load() && done)
                    done (answer);
            });
        }).detach();
    }

    UpdateCheck::Answer UpdateCheck::askNow (const std::string& urlToAsk, const std::string& running)
    {
        const juce::URL address (juce::String::fromUTF8 (urlToAsk.c_str()));

       #if JUCE_LINUX
        //  A file is read as a file everywhere; the network, on Linux, through a program (ASY).
        if (! address.isLocalFile())
            return askThroughATool (urlToAsk, running);
       #endif

        int status = -1;

        auto in = address.createInputStream (
            juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                .withExtraHeaders ("User-Agent: Go.dot/" + juce::String::fromUTF8 (running.c_str())
                                     + "\r\nAccept: application/vnd.github+json")
                .withConnectionTimeoutMs (10000)
                .withNumRedirectsToFollow (5)
                .withStatusCode (&status));

        //  A file has no status: read, it is the answer a test wrote.
        if (address.isLocalFile())
            status = in != nullptr ? 200 : -1;

        //  A 404 says what it says with no body to read; anything unread is no answer.
        if (in == nullptr || status != 200)
            return model::updates::answerFor (in == nullptr && status != 404 ? -1 : status, {});

        /*  READ TO THE CAP AND A BYTE PAST IT, so a reply that would not stop is
            known for what it is without being held whole (ASU): past the cap,
            `answerFor` calls it no answer. */
        juce::MemoryOutputStream reply;
        reply.writeFromInputStream (*in, static_cast<juce::int64> (model::updates::largestReply) + 1);

        return model::updates::answerFor (status, std::string_view (static_cast<const char*> (reply.getData()),
                                                                    reply.getDataSize()));
    }
}
