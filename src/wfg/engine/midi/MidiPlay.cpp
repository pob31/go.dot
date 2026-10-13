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

#include <wfg/engine/midi/MidiPlay.h>

#include <wfg/engine/clock/TickThread.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <string_view>
#include <system_error>
#include <utility>

namespace wfg::midi
{
    namespace
    {
        /*  The long sleep stops this far short of a moment, and the rest is
            the lane clock's short sleeps and yields - MtcLane.cpp's shape, for
            its reason: a scheduler wakes a thread when it gets round to it. */
        constexpr std::int64_t wakeGuardNanos = 1'500'000;

        constexpr std::int64_t aSecond = 1'000'000'000;

        bool isSpace (char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

        //  The whitespace-separated words of one line.
        std::vector<std::string_view> wordsOf (std::string_view line)
        {
            std::vector<std::string_view> words;
            std::size_t at = 0;

            while (at < line.size())
            {
                while (at < line.size() && isSpace (line[at]))
                    ++at;

                const auto from = at;

                while (at < line.size() && ! isSpace (line[at]))
                    ++at;

                if (at > from)
                    words.push_back (line.substr (from, at - from));
            }

            return words;
        }

        std::optional<std::int64_t> integerOf (std::string_view word) noexcept
        {
            std::int64_t value = 0;
            const auto* end = word.data() + word.size();
            const auto [stop, error] = std::from_chars (word.data(), end, value);

            if (error != std::errc {} || stop != end)
                return std::nullopt;

            return value;
        }

        //  One or two hex digits, as `--midi-render` writes them (`%02X`).
        std::optional<std::uint8_t> byteOf (std::string_view word) noexcept
        {
            if (word.empty() || word.size() > 2)
                return std::nullopt;

            unsigned int value = 0;
            const auto* end = word.data() + word.size();
            const auto [stop, error] = std::from_chars (word.data(), end, value, 16);

            if (error != std::errc {} || stop != end)
                return std::nullopt;

            return static_cast<std::uint8_t> (value);
        }
    }

    std::optional<std::vector<PlayLine>> parseRender (const std::string& text, std::string& why)
    {
        std::vector<PlayLine> lines;
        std::size_t from = 0;
        int number = 0;

        while (from <= text.size())
        {
            auto to = text.find ('\n', from);

            if (to == std::string::npos)
                to = text.size();

            ++number;
            const auto words = wordsOf (std::string_view (text).substr (from, to - from));
            from = to + 1;

            if (words.empty())
                continue;

            const auto said = [&why, number] (const std::string& what)
            {
                why = "line " + std::to_string (number) + " " + what;
                return std::nullopt;
            };

            if (words.size() < 3)
                return said ("is not \"sent_ns due_ns\" and the bytes in hex");

            const auto sent = integerOf (words[0]);
            const auto due = integerOf (words[1]);

            if (! sent.has_value() || ! due.has_value())
                return said ("does not begin with two whole numbers of nanoseconds");

            PlayLine line;
            line.moment = *due != 0 ? *due : *sent;

            for (std::size_t n = 2; n < words.size(); ++n)
            {
                const auto byte = byteOf (words[n]);

                if (! byte.has_value())
                    return said ("has \"" + std::string (words[n]) + "\" where a byte in hex belongs");

                line.bytes.push_back (*byte);
            }

            lines.push_back (std::move (line));
        }

        return lines;
    }

    std::optional<std::vector<PlayLine>> loadRender (const std::string& path, std::string& why)
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (juce::String::fromUTF8 (path.c_str()));

        if (! file.existsAsFile())
        {
            why = "\"" + file.getFullPathName().toStdString() + "\" does not exist";
            return std::nullopt;
        }

        juce::MemoryBlock content;

        if (! file.loadFileAsData (content))
        {
            why = "\"" + file.getFullPathName().toStdString() + "\" could not be read";
            return std::nullopt;
        }

        auto lines = parseRender (content.toString().toStdString(), why);

        if (lines.has_value() && lines->empty())
        {
            why = "\"" + file.getFullPathName().toStdString() + "\" has no message in it";
            return std::nullopt;
        }

        if (! lines.has_value())
            why = "\"" + file.getFullPathName().toStdString() + "\", " + why;

        return lines;
    }

    std::vector<std::int64_t> offsetsOf (const std::vector<PlayLine>& lines)
    {
        std::vector<std::int64_t> out;
        out.reserve (lines.size());

        for (std::size_t n = 0; n < lines.size(); ++n)
            out.push_back (n == 0 ? 0 : out.back() + std::max<std::int64_t> (0, lines[n].moment - lines[n - 1].moment));

        return out;
    }

    std::int64_t loopGapOf (const std::vector<PlayLine>& lines)
    {
        for (auto n = lines.size(); n > 1; --n)
            if (const auto step = lines[n - 1].moment - lines[n - 2].moment; step > 0)
                return step;

        return aSecond;
    }

    //==============================================================================
    MidiPlayer::MidiPlayer (std::vector<PlayLine> linesToPlay, bool loopIt, Inject injectInto,
                            std::shared_ptr<const MtcLane::Clock> clockToUse)
        : lines (std::move (linesToPlay)),
          offsets (offsetsOf (lines)),
          gap (loopGapOf (lines)),
          loop (loopIt),
          inject (std::move (injectInto)),
          clock (std::move (clockToUse))
    {
    }

    MidiPlayer::~MidiPlayer()
    {
        stop();
    }

    void MidiPlayer::start()
    {
        const std::lock_guard<std::mutex> lock { sleepMutex };

        if (running || worker.joinable())
            return;

        running = true;
        worker = std::thread ([this] { run(); });
    }

    void MidiPlayer::stop()
    {
        {
            const std::lock_guard<std::mutex> lock { sleepMutex };
            running = false;
        }

        wake.notify_all();

        if (worker.joinable())
            worker.join();
    }

    bool MidiPlayer::waitUntil (std::int64_t moment)
    {
        {
            std::unique_lock<std::mutex> lock { sleepMutex };

            for (;;)
            {
                if (! running)
                    return false;

                const auto ahead = moment - clock->now();

                if (ahead <= wakeGuardNanos)
                    break;

                wake.wait_for (lock, std::chrono::nanoseconds (ahead - wakeGuardNanos));
            }
        }

        clock->sleepUntil (moment);
        return true;
    }

    void MidiPlayer::run()
    {
        //  As punctual as the lane that sends MTC: what it plays is what a time group will be timed by.
        elevateCurrentThreadForTicking();

        if (lines.empty())
        {
            done.store (true, std::memory_order_release);
            return;
        }

        auto passBegan = clock->now();

        for (;;)
        {
            for (std::size_t n = 0; n < lines.size(); ++n)
            {
                if (! waitUntil (passBegan + offsets[n]))
                    return;

                inject (lines[n].bytes, clock->now());
                played.fetch_add (1, std::memory_order_relaxed);
            }

            if (! loop)
                break;

            passBegan += offsets.back() + gap;
        }

        done.store (true, std::memory_order_release);
    }
}
