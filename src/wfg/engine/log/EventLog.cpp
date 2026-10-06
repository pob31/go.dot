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

#include <wfg/engine/log/EventLog.h>

#include <juce_core/juce_core.h>

#include <atomic>
#include <charconv>
#include <sstream>

namespace wfg
{
    namespace
    {
        /*  Splits a line into tokens on spaces, treating a double-quoted run as
            one token, quotes and escapes included. That is what lets a cue name
            with a space in it survive a round trip through the log: the atom
            grammar quotes strings, and this is the matching reader.

            An unterminated quote yields no tokens at all, so the line is
            reported as malformed rather than half-read.  */
        std::optional<std::vector<std::string_view>> tokenise (std::string_view line)
        {
            std::vector<std::string_view> tokens;
            std::size_t i = 0;

            while (i < line.size())
            {
                while (i < line.size() && line[i] == ' ')
                    ++i;

                if (i >= line.size())
                    break;

                const std::size_t start = i;
                bool inQuotes = false;

                for (; i < line.size(); ++i)
                {
                    const char c = line[i];

                    if (inQuotes)
                    {
                        if (c == '\\')
                        {
                            ++i;                       // skip the escaped character

                            if (i >= line.size())
                                return std::nullopt;
                        }
                        else if (c == '"')
                        {
                            inQuotes = false;
                        }
                    }
                    else if (c == '"')
                    {
                        inQuotes = true;
                    }
                    else if (c == ' ')
                    {
                        break;
                    }
                }

                if (inQuotes)
                    return std::nullopt;

                tokens.push_back (line.substr (start, i - start));
            }

            return tokens;
        }

        template <typename Int>
        std::optional<Int> parseInt (std::string_view text)
        {
            Int value {};
            const auto* first = text.data();
            const auto* last = text.data() + text.size();
            const auto result = std::from_chars (first, last, value);

            if (result.ec != std::errc {} || result.ptr != last)
                return std::nullopt;

            return value;
        }

        /*  Origins, reasons and command names are single unquoted tokens by
            construction. Checking that here stops a malformed line from being
            read as a plausible one. */
        bool isPlainToken (std::string_view token)
        {
            return ! token.empty() && token.find ('"') == std::string_view::npos;
        }
    }

    //==============================================================================
    std::string LogRecord::toLine() const
    {
        std::string line;
        line.reserve (64 + args.size() * 16);

        line.push_back (static_cast<char> (kind));
        line.push_back (' ');
        line += std::to_string (tick);
        line.push_back (' ');
        line += std::to_string (seq);
        line.push_back (' ');
        line += origin;

        if (kind != Kind::applied)
        {
            line.push_back (' ');
            line += reason;
        }

        if (kind != Kind::dropped)
        {
            line.push_back (' ');
            line += command;
        }

        for (const auto& a : args)
        {
            line.push_back (' ');
            line += a.toAtom();
        }

        return line;
    }

    std::optional<LogRecord> LogRecord::fromLine (std::string_view line)
    {
        const auto tokens = tokenise (line);

        if (! tokens)
            return std::nullopt;

        const auto& t = *tokens;

        if (t.size() < 4 || t[0].size() != 1)
            return std::nullopt;

        LogRecord record;

        switch (t[0][0])
        {
            case 'A': record.kind = Kind::applied;  break;
            case 'R': record.kind = Kind::rejected; break;
            case 'X': record.kind = Kind::dropped;  break;
            default:  return std::nullopt;
        }

        const auto tick = parseInt<std::int64_t> (t[1]);
        const auto seq = parseInt<std::uint64_t> (t[2]);

        if (! tick || ! seq || ! isPlainToken (t[3]))
            return std::nullopt;

        record.tick = *tick;
        record.seq = *seq;
        record.origin = std::string (t[3]);

        std::size_t next = 4;

        if (record.kind != Kind::applied)
        {
            if (next >= t.size() || ! isPlainToken (t[next]))
                return std::nullopt;

            record.reason = std::string (t[next++]);
        }

        if (record.kind != Kind::dropped)
        {
            if (next >= t.size() || ! isPlainToken (t[next]))
                return std::nullopt;

            record.command = std::string (t[next++]);
        }

        for (; next < t.size(); ++next)
        {
            auto value = osc::Value::fromAtom (t[next]);

            if (! value)
                return std::nullopt;

            record.args.push_back (std::move (*value));
        }

        return record;
    }

    //==============================================================================
    /*  THE LOW-PRIORITY WRITER (2026-10-06). Lines come in from any thread on
        a lock-free queue - Dmitry Vyukov's intrusive multi-producer list: a
        push is one exchange and one store, so neither the real-time tick
        thread nor the message thread ever waits on this one, and a writer
        starved by a busy machine cannot hold either of them. One thread takes
        them off, writes them in order and flushes once a batch. */
    class EventLog::Writer final : private juce::Thread
    {
    public:
        explicit Writer (std::unique_ptr<std::ofstream> streamToUse)
            : juce::Thread ("wfg log writer"), stream (std::move (streamToUse)),
              head (new Node()), tail (head.load())
        {
        }

        ~Writer() override
        {
            finish();

            while (tail != nullptr)
            {
                auto* next = tail->next.load (std::memory_order_acquire);
                delete tail;
                tail = next;
            }
        }

        bool start()    { return startThread (juce::Thread::Priority::low); }

        /** Any thread. */
        void push (std::string line)
        {
            auto* node = new Node();
            node->line = std::move (line);

            /*  Counted before it is visible, so `pending` can say one too many
                for an instant and never one too few. */
            queued.fetch_add (1, std::memory_order_relaxed);

            auto* previous = head.exchange (node, std::memory_order_acq_rel);
            previous->next.store (node, std::memory_order_release);
            wake.signal();
        }

        std::int64_t pending() const noexcept
        {
            return queued.load (std::memory_order_relaxed) - written.load (std::memory_order_acquire);
        }

        /** The closing thread: what is queued goes down, then the file closes. */
        void finish()
        {
            if (isThreadRunning())
            {
                signalThreadShouldExit();
                wake.signal();
                stopThread (-1);    // a drain, never a kill: a log cut mid-line is worse
            }

            drain();
            stream.reset();
        }

    private:
        struct Node
        {
            std::string line;
            std::atomic<Node*> next { nullptr };
        };

        void run() override
        {
            while (! threadShouldExit())
            {
                wake.wait (250);
                drain();
            }

            drain();
        }

        /** The writer thread, or the closing one once that has stopped. */
        void drain()
        {
            if (stream == nullptr)
                return;

            std::int64_t count = 0;

            for (auto* next = tail->next.load (std::memory_order_acquire); next != nullptr;
                 next = tail->next.load (std::memory_order_acquire))
            {
                *stream << next->line << '\n';
                next->line.clear();
                delete tail;
                tail = next;
                ++count;
            }

            if (count > 0)
            {
                stream->flush();
                written.fetch_add (count, std::memory_order_release);
            }
        }

        std::unique_ptr<std::ofstream> stream;
        juce::WaitableEvent wake;
        std::atomic<Node*> head;
        Node* tail;
        std::atomic<std::int64_t> queued { 0 }, written { 0 };
    };

    //==============================================================================
    EventLog::EventLog() = default;

    EventLog::~EventLog()
    {
        close();
    }

    bool EventLog::isOpen() const noexcept
    {
        return writer != nullptr || inMemory;
    }

    bool EventLog::open (const std::string& path, const std::vector<std::string>& headerLines)
    {
        close();

        /*  Binary mode, deliberately: on Windows a text-mode stream turns every
            newline into CRLF, and a log that differs byte for byte between
            platforms cannot be a fixture. */
        auto stream = std::make_unique<std::ofstream> (path, std::ios::binary | std::ios::trunc);

        if (! stream->is_open())
            return false;

        writer = std::make_unique<Writer> (std::move (stream));
        writeHeader (headerLines);

        if (! writer->start())
        {
            writer.reset();
            return false;
        }

        return true;
    }

    void EventLog::openInMemory (const std::vector<std::string>& headerLines)
    {
        close();
        inMemory = true;
        memory.clear();
        writeHeader (headerLines);
    }

    void EventLog::writeHeader (const std::vector<std::string>& headerLines)
    {
        writeLine ("# wfg-log " + std::to_string (formatVersion));

        for (const auto& h : headerLines)
            writeLine ("# " + h);
    }

    void EventLog::writeLine (std::string line)
    {
        if (writer != nullptr)
        {
            writer->push (std::move (line));
        }
        else if (inMemory)
        {
            memory += line;
            memory.push_back ('\n');
        }
    }

    void EventLog::write (const LogRecord& record)
    {
        writeLine (record.toLine());
    }

    void EventLog::note (const std::string& text)
    {
        if (writer != nullptr)
            writer->push ("# note " + text);
    }

    std::int64_t EventLog::pending() const noexcept
    {
        return writer != nullptr ? writer->pending() : 0;
    }

    void EventLog::close()
    {
        if (writer != nullptr)
        {
            writer->finish();
            writer.reset();
        }

        inMemory = false;
    }

    //==============================================================================
    LogFile LogFile::parse (std::string_view text)
    {
        LogFile result;
        std::size_t lineNumber = 0;
        std::size_t pos = 0;

        for (;;)
        {
            const auto end = text.find ('\n', pos);
            auto line = end == std::string_view::npos ? text.substr (pos)
                                                      : text.substr (pos, end - pos);

            ++lineNumber;

            if (! line.empty() && line.back() == '\r')
                line.remove_suffix (1);

            if (! line.empty())
            {
                if (line.front() == '#')
                {
                    auto header = line.substr (1);

                    if (! header.empty() && header.front() == ' ')
                        header.remove_prefix (1);

                    result.headerLines.emplace_back (header);
                }
                else if (auto record = LogRecord::fromLine (line))
                {
                    result.records.push_back (std::move (*record));
                }
                else
                {
                    result.errors.push_back ("line " + std::to_string (lineNumber)
                                             + ": malformed record: " + std::string (line));
                }
            }

            if (end == std::string_view::npos)
                break;

            pos = end + 1;
        }

        return result;
    }

    std::optional<LogFile> LogFile::read (const std::string& path)
    {
        std::ifstream stream (path, std::ios::binary);

        if (! stream.is_open())
            return std::nullopt;

        std::ostringstream buffer;
        buffer << stream.rdbuf();
        const auto text = buffer.str();

        return parse (text);
    }
}
