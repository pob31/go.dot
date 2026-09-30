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

#include <wfg/engine/app/StartupReport.h>

#include <atomic>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <streambuf>

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <io.h>
#elif defined (__APPLE__)
 #include <CoreFoundation/CoreFoundation.h>
 #include <unistd.h>
#else
 #include <unistd.h>
#endif

namespace wfg::app
{
    /*  std::cerr's own buffer, with a copy taken on the way through.

        LOCKED, because anything started before the window - a scan, a device,
        Tracktion - may write from its own thread, and two appends at once
        would be a race on the string. Never the audio thread, which does not
        log at all (PRD §4.2).

        STAYS IN PLACE UNTIL THE DESTRUCTOR, and the copying is switched off by
        a flag instead of by taking it out: swapping a stream's buffer is not
        safe against a thread that is writing to it, and a flag is. */
    struct StartupReport::Tee final : std::streambuf
    {
        explicit Tee (std::ostream& streamToCopy)
            : stream (streamToCopy), original (streamToCopy.rdbuf (this))
        {
        }

        ~Tee() override { stream.rdbuf (original); }

        int overflow (int c) override
        {
            if (traits_type::eq_int_type (c, traits_type::eof()))
                return traits_type::not_eof (c);

            const std::lock_guard<std::mutex> lock (mutex);

            if (copying.load (std::memory_order_relaxed))
                text.push_back (static_cast<char> (c));

            return original->sputc (static_cast<char> (c));
        }

        std::streamsize xsputn (const char* s, std::streamsize n) override
        {
            const std::lock_guard<std::mutex> lock (mutex);

            if (copying.load (std::memory_order_relaxed))
                text.append (s, static_cast<std::size_t> (n));

            return original->sputn (s, n);
        }

        int sync() override { return original->pubsync(); }

        std::string copied()
        {
            const std::lock_guard<std::mutex> lock (mutex);
            return text;
        }

        std::ostream& stream;
        std::streambuf* original;
        std::mutex mutex;
        std::string text;
        std::atomic<bool> copying { true };
    };

    namespace
    {
        std::atomic<StartupReport::Tee*> live { nullptr };

        /*  What the alert shows: the sentences, less the `wfg serve: ` each
            one starts with - a person who double-clicked a show did not type
            a verb - and no more than the end of a long story. */
        std::string forPeople (const std::string& copied)
        {
            static const std::string prefix = "wfg serve: ";
            std::string out;
            std::size_t start = 0;

            while (start < copied.size())
            {
                auto end = copied.find ('\n', start);
                if (end == std::string::npos)
                    end = copied.size();

                auto line = copied.substr (start, end - start);

                if (! line.empty() && line.back() == '\r')
                    line.pop_back();

                if (line.rfind (prefix, 0) == 0)
                    line.erase (0, prefix.size());

                if (! line.empty())
                    out += line + "\n";

                start = end + 1;
            }

            constexpr std::size_t most = 1500;

            if (out.size() > most)
                out = "...\n" + out.substr (out.size() - most);

            return out.empty() ? std::string ("It stopped before its window opened, and did not say why.")
                               : out;
        }
    }

    StartupReport::StartupReport()
        : tee (std::make_unique<Tee> (std::cerr))
    {
        live.store (tee.get());
    }

    StartupReport::~StartupReport()
    {
        live.store (nullptr);
    }

    void StartupReport::windowIsUp() noexcept
    {
        if (auto* report = live.load())
            report->copying.store (false);
    }

    void StartupReport::showIfFailed (int exitCode)
    {
        if (exitCode == 0 || ! tee->copying.load() || stderrIsATerminal())
            return;

        showAlert ("Go.dot could not open the show", forPeople (tee->copied()));
    }

    bool stderrIsATerminal()
    {
       #if defined (_WIN32)
        return _isatty (_fileno (stderr)) != 0;
       #else
        return isatty (STDERR_FILENO) != 0;
       #endif
    }

    void showAlert (const std::string& title, const std::string& message)
    {
       #if defined (_WIN32)
        const auto wide = [] (const std::string& utf8)
        {
            const auto length = MultiByteToWideChar (CP_UTF8, 0, utf8.data(), static_cast<int> (utf8.size()),
                                                     nullptr, 0);
            std::wstring out (static_cast<std::size_t> (length), L'\0');
            MultiByteToWideChar (CP_UTF8, 0, utf8.data(), static_cast<int> (utf8.size()), out.data(), length);
            return out;
        };

        MessageBoxW (nullptr, wide (message).c_str(), wide (title).c_str(),
                     MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
       #elif defined (__APPLE__)
        /*  CoreFoundation's alert, not AppKit's: plain C, it needs no NSApp,
            and by now the verb that stood NSApp up has returned. */
        const auto cf = [] (const std::string& utf8)
        {
            return CFStringCreateWithCString (kCFAllocatorDefault, utf8.c_str(), kCFStringEncodingUTF8);
        };

        const auto header = cf (title);
        const auto body = cf (message);
        CFOptionFlags answered = 0;

        CFUserNotificationDisplayAlert (0, kCFUserNotificationStopAlertLevel, nullptr, nullptr, nullptr,
                                        header, body, nullptr, nullptr, nullptr, &answered);

        if (header != nullptr) CFRelease (header);
        if (body != nullptr)   CFRelease (body);
       #else
        (void) title;
        (void) message;
       #endif
    }
}
