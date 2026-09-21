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

#include <wfg/client/model/Transport.h>

#include <wfg/engine/tree/TreeSnapshot.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <tuple>

namespace wfg::client::model
{
    namespace
    {
        /*  THE DASH IS "THE ENGINE HAS NOT SAID", and it is the same dash the
            page draws, for the same reason: a reading nobody has published yet
            is not a zero and not a no. */
        constexpr const char* unsaid = "—";

        /*  `join`, which put a middle dot between two half-sentences, went with
            the last line that had two halves to put together: the transport
            lost its saved/unsaved line and its audio word on 2026-09-18, and
            what is left says one thing each. Kept out rather than kept unused -
            GCC's -Werror=unused-function is right that a helper nothing calls
            is a claim about a shape the file no longer has. */
    }

    std::string TransportReading::standbyLine() const
    {
        if (standbyId.empty())
            return "no standby";

        if (standbyKind.empty())
            return standbyName;

        return standbyName + "  " + standbyKind;
    }

    namespace
    {
        /*  ONE HALF OF THE OLD UNDO LINE, now a tooltip apiece. `—` for a
            client the engine has not answered yet, the plain refusal when
            there is nothing, and otherwise the engine's own name for what
            would be taken back. */
        std::string tipFor (Flag can, const std::string& name,
                            const char* verb, const char* nothing)
        {
            if (can == Flag::unsaid)    return std::string (verb) + ": " + unsaid;
            if (can == Flag::no)        return std::string (nothing);

            return std::string (verb) + " " + (name.empty() ? "the last edit" : name);
        }
    }

    std::string TransportReading::undoTip() const
    {
        return tipFor (canUndo, undoName, "undo", "nothing to undo");
    }

    std::string TransportReading::redoTip() const
    {
        return tipFor (canRedo, redoName, "redo", "nothing to redo");
    }

    std::size_t countWarnings (std::string_view all)
    {
        if (all.empty())
            return 0;

        /*  ONE PER LINE, so the count is the lines - and a trailing newline
            does not invent a last empty warning. */
        auto lines = std::size_t { 1 };

        for (std::size_t at = 0; at < all.size(); ++at)
            if (all[at] == '\n' && at + 1 < all.size())
                ++lines;

        return lines;
    }

    std::string firstWarning (std::string_view all)
    {
        const auto end = all.find ('\n');
        const auto first = all.substr (0, end == std::string_view::npos ? all.size() : end);

        /*  CLIPPED HERE, not by whatever draws it. One warning of a quarter of
            a megabyte is as able to hang a text layout as eighteen hundred
            short ones, and the place that knows this is a foot-of-window
            summary is here. */
        constexpr std::size_t longest = 160;

        if (first.size() > longest)
            return std::string (first.substr (0, longest)) + "…";

        return std::string (first);
    }

    std::string TransportReading::warningLine() const
    {
        if (warningCount == 0)
            return {};

        const auto count = std::to_string (warningCount)
                         + (warningCount == 1 ? " warning" : " warnings");

        return warningFirst.empty() ? count : count + " · " + warningFirst;
    }

    std::string TransportReading::errorLine() const
    {
        if (status == "noClock")
            return "Audio disconnected - cues paused; waiting for the interface and clock.";
        if (lastError.empty())
            return {};

        /*  tick, sequence, origin, reason, command - and the command may carry
            no spaces, so five fields is exactly what a well-formed record has.
            Anything else is shown as it came. */
        const auto fields = words (lastError);

        if (fields.size() != 5)
            return lastError;

        return fields[4] + " refused: " + fields[3];
    }

    std::string TransportReading::lockLine() const
    {
        /*  `unsaid` gets no word at all, because "the engine has not told us
            whether the show is locked" belongs beside the lock's own button,
            which says so by being disabled. */
        return locked == Flag::yes ? "locked" : "";
    }

    bool TransportReading::operator== (const TransportReading& other) const noexcept
    {
        const auto tie = [] (const TransportReading& r)
        {
            return std::tie (r.show, r.dirty, r.locked, r.recovery, r.recording,
                             r.tick, r.clock, r.rate,
                             r.listId, r.listName, r.standbyId, r.standbyName, r.standbyKind,
                             r.standbyNotes,
                             r.canUndo, r.canRedo, r.undoName, r.redoName,
                             r.status, r.lastError, r.writeError,
                             r.warningCount, r.warningFirst, r.revision);
        };

        return tie (*this) == tie (other);
    }

    TransportReading readTransport (const tree::TreeSnapshot& snapshot)
    {
        TransportReading reading;

        reading.show = text (snapshot, "/godot/document/name");
        reading.dirty = flag (snapshot, "/godot/document/dirty");
        reading.locked = flag (snapshot, "/godot/document/locked");
        reading.recovery = flag (snapshot, "/godot/document/recovery");
        reading.recording = flag (snapshot, "/godot/document/recording");

        reading.tick = text (snapshot, "/godot/engine/tick");
        reading.clock = text (snapshot, "/godot/engine/clock");

        const auto rate = text (snapshot, "/godot/engine/sampleRate");
        const auto block = text (snapshot, "/godot/engine/blockSize");
        reading.rate = (rate.empty() || rate == "0") ? std::string() : rate + " / " + block;

        /*  THE FOCUSED LIST, WITH THE PAGE'S FALLBACK (strip.js:223): the
            engine's `focus` when it names one, else the first list there is,
            else nothing - a show with no list has no standby to show. */
        reading.listId = text (snapshot, "/godot/list/focus");

        if (reading.listId.empty())
            if (const auto lists = words (text (snapshot, "/godot/list/order")); ! lists.empty())
                reading.listId = lists.front();

        if (! reading.listId.empty())
        {
            reading.listName = text (snapshot, "/godot/list/" + reading.listId + "/name");
            reading.standbyId = text (snapshot, "/godot/list/" + reading.listId + "/standby");
        }

        if (! reading.standbyId.empty())
        {
            reading.standbyName = text (snapshot, "/godot/cue/" + reading.standbyId + "/name");
            reading.standbyKind = text (snapshot, "/godot/cue/" + reading.standbyId + "/kind");
            reading.standbyNotes = text (snapshot, "/godot/cue/" + reading.standbyId + "/notes");
        }

        reading.canUndo = flag (snapshot, "/godot/document/canUndo");
        reading.canRedo = flag (snapshot, "/godot/document/canRedo");
        reading.undoName = text (snapshot, "/godot/document/undoName");
        reading.redoName = text (snapshot, "/godot/document/redoName");

        reading.status = text (snapshot, "/godot/audio/status");
        reading.lastError = text (snapshot, "/godot/engine/lastError");
        reading.writeError = text (snapshot, "/godot/document/writeError");
        /*  READ, SUMMARISED, AND THE LONG STRING DROPPED on the spot: nothing
            downstream of here ever holds it, so nothing downstream can be hung
            by a show with eighteen hundred things wrong with it. */
        const auto warnings = text (snapshot, "/godot/document/warnings");
        reading.warningCount = countWarnings (warnings);
        reading.warningFirst = firstWarning (warnings);

        if (const auto* node = snapshot.find ("/godot/document/revision"))
            if (const auto sole = node->soleValue(); sole.has_value() && sole->isInt64())
                reading.revision = static_cast<std::uint64_t> (sole->getInt64());

        return reading;
    }
}
