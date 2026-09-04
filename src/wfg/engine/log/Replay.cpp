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

#include <wfg/engine/log/Replay.h>

#include <algorithm>
#include <cstddef>

namespace wfg
{
    namespace
    {
        void submitRecord (Engine& engine, const LogRecord& r)
        {
            if (r.kind == LogRecord::Kind::dropped)
            {
                Drop drop;
                drop.origin = r.origin;
                drop.reason = r.reason;

                /*  The record carries the packet as a blob argument, if it was
                    kept at all. Put it back untouched; nothing parses it. */
                if (! r.args.empty() && r.args.front().isBlob())
                    drop.payload = r.args.front().getBlob().bytes;

                engine.submit (std::move (drop));
                return;
            }

            Event event;
            event.origin = r.origin;
            event.command = r.command;
            event.args = r.args;
            engine.submit (std::move (event));
        }
    }

    ReplayResult replay (Engine& engine, const LogFile& logFile)
    {
        ReplayResult result;
        result.recordsExpected = logFile.records.size();

        /*  Read the log the engine writes back, in memory, without a header:
            the header of the original names the bundle and its hashes, which
            the replay is being compared against rather than reproducing. */
        engine.log().openInMemory ({});
        engine.setLogging (true);

        /*  Records are grouped by tick, because that is how they arrived: a
            tick's worth of events is submitted, then the tick is processed. A
            log whose ticks do not ascend is not a log this engine wrote, so it
            is reported rather than sorted into shape. */
        std::size_t i = 0;

        while (i < logFile.records.size())
        {
            const auto tick = logFile.records[i].tick;
            std::size_t groupEnd = i;

            while (groupEnd < logFile.records.size() && logFile.records[groupEnd].tick == tick)
            {
                submitRecord (engine, logFile.records[groupEnd]);
                ++groupEnd;
            }

            engine.processTick (tick);
            i = groupEnd;
        }

        result.producedLog = engine.log().contents();
        engine.log().close();

        const auto produced = LogFile::parse (result.producedLog);
        result.recordsReplayed = produced.records.size();

        for (const auto& e : produced.errors)
            result.mismatches.push_back ("the replay produced an unreadable record: " + e);

        const auto common = std::min (produced.records.size(), logFile.records.size());

        for (std::size_t n = 0; n < common; ++n)
        {
            const auto expected = logFile.records[n].toLine();
            const auto actual = produced.records[n].toLine();

            if (expected != actual)
                result.mismatches.push_back ("record " + std::to_string (n)
                                             + ": expected [" + expected
                                             + "] but replay produced [" + actual + "]");
        }

        if (produced.records.size() != logFile.records.size())
            result.mismatches.push_back ("the log has " + std::to_string (logFile.records.size())
                                         + " record(s), the replay produced "
                                         + std::to_string (produced.records.size()));

        result.ok = result.mismatches.empty();
        return result;
    }
}
