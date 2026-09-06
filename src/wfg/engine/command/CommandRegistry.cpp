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

#include <wfg/engine/command/CommandRegistry.h>

#include <algorithm>

namespace wfg
{
    void CommandRegistry::add (Command command)
    {
        const auto existing = byName.find (command.name);

        if (existing != byName.end())
        {
            commands[existing->second] = std::move (command);
            return;
        }

        byName.emplace (command.name, commands.size());
        commands.push_back (std::move (command));
    }

    const Command* CommandRegistry::find (std::string_view name) const
    {
        const auto it = byName.find (name);
        return it == byName.end() ? nullptr : &commands[it->second];
    }

    //==============================================================================
    namespace
    {
        /*  Coerce a value to the declared tag, or return nullopt when the two
            are not compatible. The numeric family (i h f d) converts within
            itself and nowhere else; a bool parameter accepts T, F and an int.  */
        std::optional<osc::Value> coerceImpl (char declared, const osc::Value& v)
        {
            switch (declared)
            {
                case 'i':
                    if (v.isInt32())   return v;
                    if (v.isFloat32()) return osc::Value::int32 (static_cast<std::int32_t> (v.getFloat32()));
                    if (v.isInt64())   return osc::Value::int32 (static_cast<std::int32_t> (v.getInt64()));
                    if (v.isFloat64()) return osc::Value::int32 (static_cast<std::int32_t> (v.getFloat64()));
                    return std::nullopt;

                case 'h':
                    if (v.isInt64())   return v;
                    if (v.isInt32())   return osc::Value::int64 (v.getInt32());
                    if (v.isFloat32()) return osc::Value::int64 (static_cast<std::int64_t> (v.getFloat32()));
                    if (v.isFloat64()) return osc::Value::int64 (static_cast<std::int64_t> (v.getFloat64()));
                    return std::nullopt;

                case 'f':
                    if (v.isFloat32()) return v;
                    if (v.isInt32())   return osc::Value::float32 (static_cast<float> (v.getInt32()));
                    if (v.isInt64())   return osc::Value::float32 (static_cast<float> (v.getInt64()));
                    if (v.isFloat64()) return osc::Value::float32 (static_cast<float> (v.getFloat64()));
                    return std::nullopt;

                case 'd':
                    if (v.isFloat64()) return v;
                    if (v.isNumber())  return osc::Value::float64 (v.asDouble());
                    return std::nullopt;

                case 's':
                    return v.isString() ? std::optional<osc::Value> (v) : std::nullopt;

                case 'b':
                    return v.isBlob() ? std::optional<osc::Value> (v) : std::nullopt;

                case 'T':
                    /*  A boolean parameter. T and F are its type; an int 0 or 1
                        is accepted too, because a great many OSC senders cannot
                        emit T or F at all. */
                    if (v.isBool())  return v;
                    if (v.isInt32()) return osc::Value::boolean (v.getInt32() != 0);
                    return std::nullopt;

                case '*':
                    /*  Whatever the caller sent. One command uses it and one
                        only: node.set, whose signature IS the target node's -
                        a write to a `d` node carries a double and a write to
                        an `s` node carries a string, and the registry cannot
                        know which until it has read the address. The type
                        check still happens, one layer in, against the schema
                        row the address resolves to; refusing here would mean
                        refusing every write except to string nodes. */
                    return v;

                default:
                    return std::nullopt;
            }
        }
    }

    std::optional<osc::Value> CommandRegistry::coerceToTag (char declaredTag, const osc::Value& value)
    {
        return coerceImpl (declaredTag, value);
    }

    CommandRegistry::Check CommandRegistry::checkArgs (const Command& command,
                                                       const std::vector<osc::Value>& args)
    {
        Check result;

        std::size_t required = 0;

        for (const auto& p : command.params)
            if (! p.optional)
                ++required;

        /*  THE TAIL, when the last parameter declares one: as many more of its
            type as the caller has, and no upper bound. Only the last, because a
            variadic in the middle would make "which parameter is this argument"
            a guess - and every command that wants one wants it at the end, for
            a list of identifiers a handler produced. */
        const auto variadic = ! command.params.empty() && command.params.back().variadic;

        if (args.size() < required || (! variadic && args.size() > command.params.size()))
        {
            result.reason = reason::arity;
            return result;
        }

        result.args.reserve (args.size());

        for (std::size_t i = 0; i < args.size(); ++i)
        {
            if (args[i].isNonFinite())
            {
                result.reason = reason::nonFinite;
                result.args.clear();
                return result;
            }

            const auto& declared = command.params[std::min (i, command.params.size() - 1)];

            auto coerced = coerceImpl (declared.typeTag, args[i]);

            if (! coerced)
            {
                result.reason = reason::typeMismatch;
                result.args.clear();
                return result;
            }

            result.args.push_back (std::move (*coerced));
        }

        result.ok = true;
        return result;
    }
}
