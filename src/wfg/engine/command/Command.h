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
    A named command (PRD §3.2, §4.11): "every gesture-reachable action exists as
    a named command, so it can be bound to a button, called over OSC, or found
    in a menu". The registry is the complete list; the OSCQuery surface, the
    event log, the CLI and the replay tool are all projections of it.

    A handler runs on the tick thread with the arguments already checked
    against the signature (arity and types, with i<->f coerced). It returns an
    Outcome; the engine writes the log record from that, never from the
    submitted event, so that what the log holds is what was APPLIED — with any
    id the handler generated in place of the argument the caller left out.
*/

#include <wfg/engine/osc/OscValue.h>

#include <functional>
#include <string>
#include <vector>

namespace wfg
{
    struct CommandParam
    {
        std::string name;
        /*  One OSC type tag: i h f d s b T (a bool accepts T or F), or '*'
            for "whatever the target declares" - see the note in
            CommandRegistry.cpp, and node.set, which is the only user. */
        char typeTag = 's';
        bool optional = false;    // optional params come last and may be omitted
    };

    struct Outcome
    {
        bool applied = false;
        std::string reason;                    // a reason code when not applied, e.g. "unknown-id"
        std::vector<osc::Value> appliedArgs;   // the arguments as applied, for the log

        static Outcome ok (std::vector<osc::Value> args)
        {
            Outcome o;
            o.applied = true;
            o.appliedArgs = std::move (args);
            return o;
        }

        static Outcome rejected (std::string reasonCode)
        {
            Outcome o;
            o.reason = std::move (reasonCode);
            return o;
        }
    };

    /*  What a handler is given. Phase 1 grows this (the document, the tree, the
        lists) one PR at a time; the skeleton only knows the tick. It is a struct
        of references so that a handler can be unit-tested with a hand-built one. */
    struct CommandContext
    {
        std::int64_t tick = 0;
        const std::string* origin = nullptr;
    };

    using CommandHandler = std::function<Outcome (CommandContext&, const std::vector<osc::Value>& args)>;

    struct Command
    {
        std::string name;                  // dotted, lower case: "standby.next"
        std::string description;
        std::vector<CommandParam> params;
        bool mutates = true;               // false for pure queries; the log records both
        CommandHandler handler;
    };

    /*  Reason codes are part of the log format and therefore a contract; keep
        them here, in one place, spelled exactly as the log spells them. */
    namespace reason
    {
        inline constexpr const char* unknownCommand  = "unknown-command";
        inline constexpr const char* arity           = "arity";
        inline constexpr const char* typeMismatch    = "type-mismatch";
        inline constexpr const char* nonFinite       = "non-finite";
        inline constexpr const char* unknownId       = "unknown-id";

        /*  The cue exists, is in this list, and is somewhere the pointer may
            not stand: inside a timeline or an automatic group, or in a header
            or a footer.

            ITS OWN CODE rather than `not-in-list`, because it sends somebody
            somewhere different. `not-in-list` means "you named the wrong list";
            this means "that cue is one the MACHINE advances" (§3.5 - only GO
            moves standby, and a pointer the scheduler also moved would be two
            things moving one pointer). The remedy is to make the group manual,
            or to park on the group instead. */
        inline constexpr const char* notManualPath   = "not-manual-path";

        /*  A manual sequence group was asked to run by something that is not
            GO - `cue.fire`, or a trigger.

            §3.6 makes the OPERATOR the parent of a manual group: its members
            start on GO, one press at a time. Fired by name there is nobody to
            press anything, so it would run its header, start its first member
            and then wait for a GO that is never coming - a scene stuck halfway
            with its voices held.

            Refused rather than quietly promoted to automatic, because "run this
            group without me" is a thing somebody may well want and is a
            different group from the one they wrote. */
        inline constexpr const char* needsGo          = "needs-go";
        inline constexpr const char* badAddress      = "bad-address";
        inline constexpr const char* readOnly        = "read-only";
        inline constexpr const char* notInList       = "not-in-list";
        inline constexpr const char* retiredId       = "retired-id";
        inline constexpr const char* malformedPacket = "malformed-packet";

        /*  A mount's namespace file could not be read, or is not a usable
            OSCQuery description. Distinct from bad-address on purpose: the
            mount exists and was named correctly, and what failed is the file it
            points at - which is somebody else's, and is the thing to go and
            look at. */
        inline constexpr const char* badNamespace   = "bad-namespace";
    }
}
