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

/*
    The log format, read and written.

    A serialisation surface, so these run under fr_FR as well as C. The lines
    below are written out by hand rather than produced by the writer under test:
    a round trip through one implementation proves only that it agrees with
    itself, which is the failure mode this file exists to catch.
*/

#include <3rd_party/doctest/tracktion_doctest.hpp>

#include "TestSupport.h"

#include <wfg/engine/log/EventLog.h>

#include <string>
#include <vector>

using namespace wfg;

//==============================================================================
TEST_CASE ("log record: an applied record reads exactly as written")
{
    INFO ("locale in effect: " << std::string (wfgtest::appliedLocaleName()));

    const std::string line =
        R"(A 12 34 ws:192.168.1.20:51234 node.set s:"/godot/cues/B3N8R5TW/name" s:"House to half")";

    const auto record = LogRecord::fromLine (line);
    REQUIRE (record.has_value());

    CHECK (record->kind == LogRecord::Kind::applied);
    CHECK (record->tick == 12);
    CHECK (record->seq == 34);
    CHECK (record->origin == "ws:192.168.1.20:51234");
    CHECK (record->command == "node.set");
    CHECK (record->reason.empty());
    REQUIRE (record->args.size() == 2);
    CHECK (record->args[0].getString() == "/godot/cues/B3N8R5TW/name");
    CHECK (record->args[1].getString() == "House to half");

    // And back out again, byte for byte.
    CHECK (record->toLine() == line);
}

TEST_CASE ("log record: a rejected record carries its reason before the command")
{
    const std::string line =
        R"(R 13 35 udp:192.168.1.7:9000 read-only node.set s:"/godot/cues/B3N8R5TW/kind" s:"group")";

    const auto record = LogRecord::fromLine (line);
    REQUIRE (record.has_value());

    CHECK (record->kind == LogRecord::Kind::rejected);
    CHECK (record->reason == "read-only");
    CHECK (record->command == "node.set");
    CHECK (record->args.size() == 2);
    CHECK (record->toLine() == line);
}

TEST_CASE ("log record: a dropped record has a reason and no command")
{
    const std::string line = "X 13 36 udp:192.168.1.7:9000 malformed-packet b:LyIvAAAsZgAA";

    const auto record = LogRecord::fromLine (line);
    REQUIRE (record.has_value());

    CHECK (record->kind == LogRecord::Kind::dropped);
    CHECK (record->reason == "malformed-packet");
    CHECK (record->command.empty());
    REQUIRE (record->args.size() == 1);
    CHECK (record->args[0].isBlob());
    CHECK (record->toLine() == line);
}

TEST_CASE ("log record: a record with no arguments")
{
    const std::string line = "A 40 41 cli standby.next";

    const auto record = LogRecord::fromLine (line);
    REQUIRE (record.has_value());
    CHECK (record->args.empty());
    CHECK (record->toLine() == line);
}

//==============================================================================
TEST_CASE ("log record: a malformed line is refused rather than half-read")
{
    const std::vector<std::string> bad {
        "",
        "A",
        "A 1",
        "A 1 2",
        "Q 1 2 cli noop",                        // unknown kind
        "AA 1 2 cli noop",                       // kind is one character
        "A x 2 cli noop",                        // tick is not a number
        "A 1 x cli noop",                        // sequence is not a number
        "A 1 2 cli",                             // applied with no command
        "R 1 2 cli read-only",                   // rejected with no command
        "X 1 2 cli",                             // dropped with no reason
        R"(A 1 2 cli node.set s:"unterminated)",  // an unterminated quote
        "A 1 2 cli node.set q:1"                  // an unknown atom
    };

    for (const auto& line : bad)
    {
        INFO ("line: " << line);
        CHECK_FALSE (LogRecord::fromLine (line).has_value());
    }
}

TEST_CASE ("log record: a string argument keeps its spaces through the tokeniser")
{
    /*  The one thing a naive whitespace split gets wrong, and the reason the
        reader has to understand the quoting the atom grammar produces. */
    LogRecord record;
    record.kind = LogRecord::Kind::applied;
    record.tick = 1;
    record.seq = 2;
    record.origin = "cli";
    record.command = "cue.create";
    record.args = { osc::Value::string ("a name with spaces"),
                    osc::Value::string (R"(and "quotes" too)"),
                    osc::Value::int32 (7) };

    const auto line = record.toLine();
    INFO ("line: " << line);

    const auto back = LogRecord::fromLine (line);
    REQUIRE (back.has_value());
    REQUIRE (back->args.size() == 3);
    CHECK (back->args[0].getString() == "a name with spaces");
    CHECK (back->args[1].getString() == R"(and "quotes" too)");
    CHECK (back->args[2].getInt32() == 7);
}

//==============================================================================
TEST_CASE ("log file: headers, records and errors are separated")
{
    const std::string text =
        "# wfg-log 1\n"
        "# bundle MyShow sha256:abc123\n"
        "A 0 0 cli noop\n"
        "\n"
        "this line is not a record\n"
        "A 1 1 cli noop\n";

    const auto parsed = LogFile::parse (text);

    REQUIRE (parsed.headerLines.size() == 2);
    CHECK (parsed.headerLines[0] == "wfg-log 1");
    CHECK (parsed.headerLines[1] == "bundle MyShow sha256:abc123");

    REQUIRE (parsed.records.size() == 2);
    CHECK (parsed.records[0].tick == 0);
    CHECK (parsed.records[1].tick == 1);

    // The unreadable line is reported with its number rather than skipped: a
    // log with a hole in it must not replay as though it were complete.
    REQUIRE (parsed.errors.size() == 1);
    CHECK (parsed.errors[0].find ("line 5") != std::string::npos);
}

TEST_CASE ("log file: CRLF line endings read the same as LF")
{
    /*  The writer opens in binary mode so it never emits CRLF, but a log that
        has been through a Windows editor, or a diff tool, still has to read. */
    const auto lf   = LogFile::parse ("# wfg-log 1\nA 0 0 cli noop\n");
    const auto crlf = LogFile::parse ("# wfg-log 1\r\nA 0 0 cli noop\r\n");

    REQUIRE (lf.records.size() == 1);
    REQUIRE (crlf.records.size() == 1);
    CHECK (lf.records[0].toLine() == crlf.records[0].toLine());
    CHECK (crlf.headerLines[0] == "wfg-log 1");
    CHECK (crlf.errors.empty());
}

TEST_CASE ("log file: a file with no trailing newline still yields its last record")
{
    const auto parsed = LogFile::parse ("# wfg-log 1\nA 0 0 cli noop");

    CHECK (parsed.errors.empty());
    REQUIRE (parsed.records.size() == 1);
    CHECK (parsed.records[0].command == "noop");
}

//==============================================================================
TEST_CASE ("event log: the writer emits its format header and one line per record")
{
    EventLog log;
    log.openInMemory ({ "bundle MyShow sha256:abc123", "clock sampleRate=48000" });

    LogRecord record;
    record.kind = LogRecord::Kind::applied;
    record.tick = 7;
    record.seq = 0;
    record.origin = "cli";      // the literal on purpose: the format is the contract
    record.command = "noop";

    log.write (record);

    const auto expected = std::string ("# wfg-log 1\n")
                        + "# bundle MyShow sha256:abc123\n"
                        + "# clock sampleRate=48000\n"
                        + "A 7 0 cli noop\n";

    CHECK (log.contents() == expected);

    // No stray carriage returns, on any platform: a fixture that differs
    // between Windows and Linux is not a fixture.
    CHECK (log.contents().find ('\r') == std::string::npos);
}
