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

        /** The line this record writes, without its newline. */
        std::string toLine() const;

        /** nullopt for a malformed line. Comments and blank lines are not
            records and are not offered here; see LogFile. */
        static std::optional<LogRecord> fromLine (std::string_view line);
    };

    /*  The writer. Opened once, flushed after every record: a log whose last
        seconds are missing after a crash is precisely the log nobody needed.
        Costs one fflush per event, at control rate, off the audio path. */
    class EventLog
    {
    public:
        EventLog() = default;

        /** Starts a new file, writing the format header. `headerLines` are the
            further `# ` lines the caller wants recorded - the bundle and its
            hashes, the clock parameters - each without its leading hash. */
        bool open (const std::string& path, const std::vector<std::string>& headerLines);

        /** Writes into memory instead of a file, for tests. */
        void openInMemory (const std::vector<std::string>& headerLines);

        bool isOpen() const noexcept { return file != nullptr || inMemory; }

        void write (const LogRecord& record);

        void close();

        /** In-memory mode only: everything written so far. */
        const std::string& contents() const noexcept { return memory; }

        static constexpr int formatVersion = 1;

    private:
        void writeHeader (const std::vector<std::string>& headerLines);
        void writeLine (const std::string& line);

        std::unique_ptr<std::ofstream> file;
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
