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
    Deterministic replay: feed a log back into a fresh engine and require it to
    write the same log again, record for record.

    That is a stronger claim than "the same end state", and it is the one worth
    testing. Two engines can arrive at the same document by different routes; a
    log that reproduces itself proves the ordering, the tick stamping, the
    rejections and the generated identifiers all came out the same way. It is
    also the property the redundancy link of PRD 3.15 will need, since a backup
    engine is nothing but a second consumer of this stream.

    How the records are fed back:
      * A and R records are re-submitted as events, with the arguments the log
        recorded - which are the arguments AS APPLIED, so a generated id is
        supplied rather than generated again. An R record must be refused again,
        with the same reason.
      * X records are re-submitted as drops. Their packet is never re-parsed
        (that would test the parser, not the engine); the record is simply put
        back into the stream so the sequence numbering stays aligned.

    Records are grouped by tick and submitted together, then processTick is
    called once for that tick - the same shape the live engine sees.
*/

#include <wfg/engine/Engine.h>
#include <wfg/engine/log/EventLog.h>

#include <string>
#include <vector>

namespace wfg
{
    struct ReplayResult
    {
        bool ok = false;
        std::size_t recordsReplayed = 0;
        std::size_t recordsExpected = 0;

        /** One line per divergence, in file order, naming both sides. */
        std::vector<std::string> mismatches;

        /** The log the replay produced, for a caller that wants to write it. */
        std::string producedLog;
    };

    /*  Replays into `engine`, which must be freshly constructed and carry the
        same command set as the session that produced the log. The engine's own
        logging is redirected into memory for the duration and restored after,
        so a replay never appends to the log it is reading. */
    ReplayResult replay (Engine& engine, const LogFile& logFile);
}
