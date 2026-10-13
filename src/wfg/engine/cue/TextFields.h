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
    THE FIELDS A TEXT CUE'S BLANKS ARE FILLED FROM (namespace draft 61, AKC,
    AKM): `/godot/text/<name>`, written from outside - a datagram, a client,
    the window - and read wherever words hold `{name}`.

    TONIGHT'S, NEVER THE SHOW'S (PRD §4.10): a score is what the evening did,
    not what somebody decided, so the values are never saved, never an undo
    step and never make the show dirty; closing the show or the engine forgets
    them. ACCEPTED UNDER THE LOCK, because the document is never touched - which
    is the point of them. LOGGED as the `node.set` they arrive as, so a replay
    fills the same blanks; a following backup ignores its own datagrams (AGD)
    and applies the main's records; one seated late is handed the values in the
    position (RD.8). Esc, a double Esc and Doh! leave them alone: they are the
    outside world's, not a cue's.

    A VALUE is the message's first argument as text - a number written without
    the locale - cut at 1024 bytes, at a character. At most 256 names; a 257th is
    refused. The tick thread's alone: the door, the Runner and the tree all run
    there.
*/

#include <wfg/engine/document/DocumentCommands.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::cue
{
    class TextFields
    {
    public:
        static constexpr std::size_t valueBytes = 1024;
        static constexpr std::size_t mostNames = 256;

        enum class Set { written, badName, full };

        /*  `value` for `name`, cut to `valueBytes`; the revision moves only
            when a value changes. */
        Set set (const std::string& name, const std::string& value);

        /*  What `name` holds, empty for a field no value has reached. */
        std::string valueOf (const std::string& name) const;

        /*  Moves with every change: what the Runner and the tree compare. */
        std::uint64_t revision() const noexcept   { return changes; }

        const std::map<std::string, std::string>& all() const noexcept   { return values; }

        /*  EVERY VALUE AT ONCE, as a seat hands them over (RD.8): the leader's
            replace this machine's, a field the leader has not got forgotten. */
        void replaceAll (std::map<std::string, std::string> leaders);

        /*  Forgotten, as a show closing forgets them. */
        void clear();

    private:
        std::map<std::string, std::string> values;
        std::uint64_t changes = 0;
    };

    /*  The prefix of a field's address. */
    constexpr std::string_view textFieldPrefix = "/godot/text/";

    /*  Whether `address` names a field: a `node.set` there is answered by the
        door below and never by the document. */
    bool isTextFieldAddress (std::string_view address) noexcept;

    /*  THE DOOR, for `node.set`'s chain (`eitherOf`): nothing for an address
        that is not a field; for one that is, applied - or `bad-address` for a
        name that does not read, `bad-value` when 256 are held and this one is
        new. Bound to the table it writes. */
    doc::LiveWrite textFieldWriteFor (TextFields& fields);
}
