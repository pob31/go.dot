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
    The complete list of things Go.dot can be asked to do (PRD 4.11).

    Registration happens once, at construction, before any thread but the
    builder's can see the engine; lookup is const and happens on the tick
    thread. There is deliberately no unregister and no runtime mutation: a
    command set that changes under a client's feet would make the OSCQuery
    namespace and the event log lie about each other.

    Ordering is insertion order, not alphabetical, so `wfg commands` and the
    /godot/cmd namespace list themselves in the order a human grouped them.
*/

#include <wfg/engine/command/Command.h>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg
{
    class CommandRegistry
    {
    public:
        CommandRegistry() = default;

        /** Adds a command. Replaces any command of the same name - the last
            registration wins, which is what lets a later phase specialise an
            earlier phase's placeholder without two entries claiming one name. */
        void add (Command command);

        /** nullptr when there is no such command. */
        const Command* find (std::string_view name) const;

        /** Every command, in registration order. */
        const std::vector<Command>& all() const noexcept { return commands; }

        std::size_t size() const noexcept { return commands.size(); }

        /*  Checks an argument list against a signature and returns the list to
            hand the handler, or a reason code.

            Two rules, and they are the whole of the type policy:
              * arity - every required parameter present, and no extras unless
                the last parameter is variadic, in which case the tail is as
                long as it likes and every value in it takes that parameter's
                type;
              * types - exact, except that the numeric tags coerce to each other,
                because a client that types 1 where 1.0 is wanted is not making a
                mistake anyone wants reported. Everything else is a type-mismatch,
                including a string where a number is wanted: silently parsing "12"
                as 12 is how a typo becomes a cue.

            Non-finite floats are refused here, once, so no later stage has to. */
        struct Check
        {
            bool ok = false;
            std::string reason;
            std::vector<osc::Value> args;   // coerced to the declared types
        };

        static Check checkArgs (const Command& command, const std::vector<osc::Value>& args);

        /*  One value against one declared OSC type tag, coerced where the
            rejection rules allow it and nullopt where they do not: the numeric
            family converts within itself, a boolean accepts an int 0 or 1
            because a great many senders cannot emit T or F, and `*` takes
            whatever it was given.

            Public because a mounted node has a declared type too, and a second
            copy of these rules living in the mount reader is a second copy that
            would eventually disagree with the log's idea of what a
            type-mismatch is. */
        static std::optional<osc::Value> coerceToTag (char declaredTag, const osc::Value& value);

    private:
        std::vector<Command> commands;
        std::map<std::string, std::size_t, std::less<>> byName;
    };
}
