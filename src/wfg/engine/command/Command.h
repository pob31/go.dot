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

        /*  AS MANY AS THERE ARE, of this type, and only on the LAST parameter.

            A few commands answer with a number of identifiers that depends on
            what they found rather than on their signature: one GO on a member
            three manual groups deep creates a run for each group it entered,
            and the record has to carry all of them because a replay never draws
            an identifier of its own. A fixed arity cannot describe that, and a
            command whose own record fails its own arity check is a session that
            cannot reproduce itself - which is exactly what `go` did, silently,
            for anybody whose show nested one manual group inside another.

            `optional` still says whether the tail may be EMPTY. Variadic and
            required together means one or more. */
        bool variadic = false;
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
        /*  The argument's TYPE was right and its VALUE is not one this command
            accepts - a scope that is neither "round" nor "group", a stop verb
            nobody has heard of.

            Its own code rather than `type-mismatch`, which says the wrong KIND
            of thing arrived and sends somebody to look at their encoder. This
            one says the right kind of thing arrived carrying a word that means
            nothing here, and the remedy is to read the list. */
        inline constexpr const char* badValue        = "bad-value";
        inline constexpr const char* badAddress      = "bad-address";
        inline constexpr const char* readOnly        = "read-only";
        inline constexpr const char* notInList       = "not-in-list";
        inline constexpr const char* retiredId       = "retired-id";
        inline constexpr const char* malformedPacket = "malformed-packet";

        /*  The bytes did not reach the disk: a full volume, a folder that went
            away, a replace the platform refused. The command was well formed
            and everything it named was found; what failed is the writing.

            Its own code rather than `bad-address`, which `document.save` said
            for this until Phase 5 and which is a lie about a full disk - it
            sends somebody to check the path they typed, and the path was fine.
            The distinction a client actually needs is between a refusal it can
            fix by sending something else and one it cannot, and this is the
            second kind: nothing about the request will make it succeed until
            something outside Go.dot changes.

            It does NOT mean the show was left half written and it does not
            promise the opposite either; what survives on disk is the writer's
            business to say, not this word's. */
        inline constexpr const char* writeFailed     = "write-failed";

        /*  Show mode is on, and the command would have changed the show half
            of the document: a create, a delete, a move, or a write to a value
            that persists in show.xml.

            Its own code rather than `read-only`, which says the NODE can never
            be written and sends somebody to read the table. This one says the
            node is writable and the SHOW is fixed, which is a fact about
            tonight rather than about the address - and the remedy is one write
            to /godot/document/locked, which is the operator's to make.

            Said by the document's four doors, so a command added next year is
            refused without having to know the lock exists. The commands that
            change the show without knocking at a door - undo, redo, revert and
            recover, when they arrive - will have to say it in their own
            handlers, and namespace draft §14.11 says why. What it never covers
            is where the operator is standing: GO, the standby, the focus and a
            mounted write reach no door that says it. */
        inline constexpr const char* locked          = "locked";

        /*  THE STACK IS EMPTY, and that is not the same fact as the show being
            unedited: a `document.revert`, a `document.recover` or a bundle load
            clears the history without clearing the show, and an edit that wrote
            nothing - a value set to what it already was - never reached it.

            Its own code rather than `bad-value`, because nothing about the
            request is wrong: `undo document` is the same words it was a moment
            ago, and what changed is that there is nothing left behind it. A
            client greys its menu item from `/godot/document/canUndo` and never
            has to read this; it exists so that the one which does not is told
            plainly rather than shown a refusal about its arguments. */
        inline constexpr const char* nothingToUndo   = "nothing-to-undo";

        /*  Nothing has been undone, or an edit since has cleared the redo half.

            Two causes and one word, because the remedy is the same: there is no
            forward history to walk. The second is the one that surprises people
            - undoing three edits and then typing anything at all discards the
            three, which is how every editor behaves and is worth saying here
            rather than in the client that has to explain it. */
        inline constexpr const char* nothingToRedo   = "nothing-to-redo";

        /*  A mount's namespace file could not be read, or is not a usable
            OSCQuery description. Distinct from bad-address on purpose: the
            mount exists and was named correctly, and what failed is the file it
            points at - which is somebody else's, and is the thing to go and
            look at. */
        inline constexpr const char* badNamespace   = "bad-namespace";
    }
}
