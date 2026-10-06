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
    The flight recorder (PRD 3.15), built before there is anything to record
    because everything else is built on top of it: deterministic replay, the
    regression fixtures, and eventually the redundancy link, which is just a
    second consumer of this stream.

    One line per record, text, append-only:

        A <tick> <seq> <origin> <command> <atoms...>
        R <tick> <seq> <origin> <reason> <command> <atoms...>
        X <tick> <seq> <origin> <reason> <blob-atom>

    A applied, R rejected, X a packet that never became a command. `seq` is
    monotonic across all three kinds, so a rejection cannot be mistaken for a
    gap. Records carry the arguments AS APPLIED - a generated id appears in the
    record of the command that generated it - so replay never needs randomness
    and never has to guess.

    Text, not binary, for the same reason the show document is XML: a log that
    can be read, diffed and quoted in a bug report is worth more than the bytes
    it saves. Atoms are the OSC value grammar of osc::Value::toAtom.
*/

#include <wfg/engine/osc/OscValue.h>

#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg
{
    struct LogRecord
    {
        enum class Kind : char { applied = 'A', rejected = 'R', dropped = 'X' };

        Kind kind = Kind::applied;
        std::int64_t tick = 0;
        std::uint64_t seq = 0;
        std::string origin;
        std::string reason;                 // rejected and dropped only
        std::string command;                // applied and rejected only
        std::vector<osc::Value> args;

        /*  A refusal's `Outcome::detail` - the address a `node.setMany` was
            refused at - carried to `/godot/engine/lastError` and NEVER to the
            line: `toLine` does not write it and `fromLine` does not read it,
            so the log's format is what it was. */
        std::string detail;

        /** The line this record writes, without its newline. */
        std::string toLine() const;

        /** nullopt for a malformed line. Comments and blank lines are not
            records and are not offered here; see LogFile. */
        static std::optional<LogRecord> fromLine (std::string_view line);
    };

    /*  The writer. Opened once.

        A FILE IS WRITTEN BY A LOW-PRIORITY THREAD OF ITS OWN (2026-10-06, the
        author: "add the engine log as a low priority thread"). The records are
        made on the tick thread, which since that day runs at real-time
        priority, and a write and a flush there were a disk - an antivirus, a
        sleeping drive - between the show's clock and its next tick. `write`
        formats the line and queues it, lock-free, and wakes the writer; the
        writer puts down everything queued and flushes once. A log whose last
        seconds are missing after a crash is still the log nobody needed, so
        the writer flushes as soon as it is woken: what a crash can lose is
        what was queued in the moment before it, not a buffer's worth.

        IN MEMORY, for tests and a replay, it stays synchronous: the contents
        are read back on the thread that wrote them. */
    class EventLog
    {
    public:
        EventLog();
        ~EventLog();

        EventLog (const EventLog&) = delete;
        EventLog& operator= (const EventLog&) = delete;

        /** Starts a new file, writing the format header. `headerLines` are the
            further `# ` lines the caller wants recorded - the bundle and its
            hashes, the clock parameters - each without its leading hash. */
        bool open (const std::string& path, const std::vector<std::string>& headerLines);

        /** Writes into memory instead of a file, for tests. */
        void openInMemory (const std::vector<std::string>& headerLines);

        bool isOpen() const noexcept;

        /** The tick thread's. Queues the record's line for the writer. */
        void write (const LogRecord& record);

        /*  A REMARK, NOT A RECORD (2026-10-06): `# note <text>` between the
            records, for what the machine did that nobody decided - an audio
            interface that went quiet for 300 ms, a tick that ran late. A
            reader takes it for a header line and a replay ignores it, so the
            records and their replay are what they were. Any thread; a file log
            only - a log in memory has nobody to read a remark. */
        void note (const std::string& text);

        /** Finishes what is queued, then closes. */
        void close();

        /*  Lines queued and not yet flushed to the file: what a process killed
            now would lose. Published as `/godot/engine/logPending`, which the
            black-box harness waits on before it stops a server (on Windows
            terminate() runs no destructor). */
        std::int64_t pending() const noexcept;

        /** In-memory mode only: everything written so far. */
        const std::string& contents() const noexcept { return memory; }

        static constexpr int formatVersion = 1;

    private:
        void writeHeader (const std::vector<std::string>& headerLines);
        void writeLine (std::string line);

        class Writer;
        std::unique_ptr<Writer> writer;
        bool inMemory = false;
        std::string memory;
    };

    /*  The reader: a parsed log, header lines and records, in file order. */
    struct LogFile
    {
        std::vector<std::string> headerLines;   // without the leading hash
        std::vector<LogRecord> records;
        std::vector<std::string> errors;        // one per unparseable line, with its number

        static LogFile parse (std::string_view text);
        static std::optional<LogFile> read (const std::string& path);
    };
}
