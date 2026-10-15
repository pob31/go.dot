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

#include <wfg/engine/process/LuaLanguage.h>

extern "C"
{
   #include <lua.h>
   #include <lauxlib.h>
   #include <lualib.h>
}

#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace wfg::process
{
    namespace
    {
        /*  THE PRELUDE (AMR): `godot`, as namespace draft §66.3 lists it, in
            Lua. Handed the native door - what a script sends, what it prints,
            the addresses it hears - and the interrupt's mark, and returns the
            function the instance calls once a tick. The door stays in its
            locals, out of the script's sight. */
        constexpr const char* prelude = R"lua(
local hostOut, hostPrint, hostHears, interrupted = ...

local rawpcall, rawxpcall, rawload, rawresume = pcall, xpcall, load, coroutine.resume

local hears = {}
local order = {}
local everything = {}
local midiHears = {}
local everyTick = {}
local timers = {}
local nextTimer = 1
local ticks = 0

local function problem (err)
    local text = tostring (err)
    local line, message = string.match (text, '^[^:]*:(%d+): (.*)$')
    if line then
        return message .. ' (line ' .. line .. ')'
    end
    return text
end

local function passOn (ok, ...)
    if not ok and (...) == interrupted then
        error (interrupted, 0)
    end
    return ok, ...
end

local function call (fn, ...)
    local ok, err = rawpcall (fn, ...)
    if not ok then
        if err == interrupted then
            error (interrupted, 0)
        end
        hostPrint (problem (err))
    end
end

local function need (yes, what)
    if not yes then
        error (what, 3)
    end
end

local function toTicks (seconds, what)
    local n = tonumber (seconds)
    need (n ~= nil and n >= 0 and n < math.huge, what .. ' needs a time in seconds')
    return math.max (1, math.floor (n * 50 + 0.5))
end

local function timer (seconds, fn, again, what)
    need (type (fn) == 'function', what .. ' needs a function')
    local every = toTicks (seconds, what)
    local id = nextTimer
    nextTimer = nextTimer + 1
    timers[id] = { due = ticks + every, every = again and every or 0, fn = fn }
    return id
end

local function refuse ()
    error ('godot cannot be changed', 2)
end

local midi = setmetatable ({}, { __index = {
    hear = function (fn)
        need (type (fn) == 'function', 'godot.midi.hear needs a function')
        midiHears[#midiHears + 1] = fn
    end,
    send = function (...)
        hostOut ('#midi', ...)
    end,
}, __newindex = refuse, __metatable = false })

local api = {
    self = 'self',
    midi = midi,
    hear = function (address, fn)
        need (type (address) == 'string' and #address > 0, 'godot.hear needs an address')
        need (type (fn) == 'function', 'godot.hear needs a function')
        local list = hears[address]
        if list == nil then
            list = {}
            hears[address] = list
            order[#order + 1] = address
            hostHears (table.unpack (order))
        end
        list[#list + 1] = fn
    end,
    hearAll = function (fn)
        need (type (fn) == 'function', 'godot.hearAll needs a function')
        everything[#everything + 1] = fn
    end,
    send = function (address, ...)
        need (type (address) == 'string' and #address > 0, 'godot.send needs an address')
        hostOut (address, ...)
    end,
    set = function (address, ...)
        need (type (address) == 'string' and #address > 0, 'godot.set needs an address')
        if string.sub (address, 1, 7) ~= '/godot/' then
            address = '/godot/' .. (string.gsub (address, '^/+', ''))
        end
        hostOut (address, ...)
    end,
    command = function (name, ...)
        need (type (name) == 'string' and #name > 0, 'godot.command needs a command')
        local path = string.gsub ((string.gsub (name, '^/+', '')), '%.', '/')
        hostOut ('/godot/cmd/' .. path, ...)
    end,
    fire = function (cue)
        hostOut ('/godot/cmd/cue/fire', cue)
    end,
    tick = function (fn)
        need (type (fn) == 'function', 'godot.tick needs a function')
        everyTick[#everyTick + 1] = fn
    end,
    after = function (seconds, fn) return timer (seconds, fn, false, 'godot.after') end,
    every = function (seconds, fn) return timer (seconds, fn, true, 'godot.every') end,
    cancel = function (id) timers[id] = nil end,
}

godot = setmetatable ({}, { __index = api, __newindex = refuse, __metatable = false })

print = function (...)
    local parts = table.pack (...)
    for i = 1, parts.n do
        parts[i] = tostring (parts[i])
    end
    hostPrint (table.concat (parts, ' ', 1, parts.n))
end

-- What a script cannot catch, and what it cannot load.
pcall = function (fn, ...) return passOn (rawpcall (fn, ...)) end
xpcall = function (fn, handler, ...)
    return passOn (rawxpcall (fn, function (e)
        if e == interrupted then return e end
        return handler (e)
    end, ...))
end
coroutine.resume = function (co, ...) return passOn (rawresume (co, ...)) end
load = function (chunk, name, mode, env) return rawload (chunk, name, 't', env) end
dofile = nil
loadfile = nil
string.dump = nil

return function (inputs)
    ticks = ticks + 1
    for i = 1, #inputs do
        local input = inputs[i]
        local to = input[1]
        if to == '#midi' then
            for _, fn in ipairs (midiHears) do
                call (fn, table.unpack (input, 2, input.n))
            end
        else
            local list = hears[to]
            if list ~= nil then
                for _, fn in ipairs (list) do
                    call (fn, table.unpack (input, 2, input.n))
                end
            end
            for _, fn in ipairs (everything) do
                call (fn, table.unpack (input, 1, input.n))
            end
        end
    end
    for _, fn in ipairs (everyTick) do
        call (fn, ticks)
    end
    local due = {}
    for id in pairs (timers) do
        due[#due + 1] = id
    end
    table.sort (due)
    for _, id in ipairs (due) do
        local t = timers[id]
        if t ~= nil and t.due <= ticks then
            if t.every > 0 then
                t.due = t.due + t.every
            else
                timers[id] = nil
            end
            call (t.fn)
        end
    end
end
)lua";

        //  The interrupt's mark: its address, as a light userdata nobody else has.
        char interruptMark = 0;

        //  What one script's state may hold.
        constexpr std::size_t memoryCap = std::size_t { 64 } * 1024 * 1024;

        class LuaLanguage final : public ScriptLanguage
        {
        public:
            explicit LuaLanguage (ScriptOutput& outputToUse)
                : output (outputToUse)
            {
                state = lua_newstate (&LuaLanguage::allocate, this);
                if (state == nullptr)
                {
                    preludeProblem = "Lua could not be started";
                    return;
                }

                *static_cast<LuaLanguage**> (lua_getextraspace (state)) = this;
                lua_sethook (state, &LuaLanguage::checkStopped, LUA_MASKCOUNT, 1000);

                //  The libraries a script has (AMS), and no other.
                const std::pair<const char*, lua_CFunction> libraries[] {
                    { LUA_GNAME, luaopen_base }, { LUA_STRLIBNAME, luaopen_string },
                    { LUA_TABLIBNAME, luaopen_table }, { LUA_MATHLIBNAME, luaopen_math },
                    { LUA_UTF8LIBNAME, luaopen_utf8 }, { LUA_COLIBNAME, luaopen_coroutine }
                };
                for (const auto& [name, opener] : libraries)
                {
                    luaL_requiref (state, name, opener, 1);
                    lua_pop (state, 1);
                }

                if (luaL_loadbufferx (state, prelude, std::char_traits<char>::length (prelude), "=godot", "t") != LUA_OK)
                {
                    preludeProblem = "Go.dot's Lua prelude failed: " + std::string (lua_tostring (state, -1));
                    lua_pop (state, 1);
                    return;
                }

                lua_pushcfunction (state, &LuaLanguage::doorOut);
                lua_pushcfunction (state, &LuaLanguage::doorPrint);
                lua_pushcfunction (state, &LuaLanguage::doorHears);
                lua_pushlightuserdata (state, &interruptMark);

                if (lua_pcall (state, 4, 1, 0) != LUA_OK || ! lua_isfunction (state, -1))
                {
                    preludeProblem = "Go.dot's Lua prelude failed: " + std::string (luaL_tolstring (state, -1, nullptr));
                    lua_settop (state, 0);
                    return;
                }
                tickFunction = luaL_ref (state, LUA_REGISTRYINDEX);
            }

            ~LuaLanguage() override
            {
                if (state != nullptr)
                    lua_close (state);
            }

            LuaLanguage (const LuaLanguage&) = delete;
            LuaLanguage& operator= (const LuaLanguage&) = delete;

            bool load (const std::string& source, std::string& problem) override
            {
                if (! preludeProblem.empty())
                {
                    problem = preludeProblem;
                    return false;
                }

                if (luaL_loadbufferx (state, source.data(), source.size(), "=script", "t") != LUA_OK)
                {
                    problem = luaProblem (lua_tostring (state, -1));
                    lua_settop (state, 0);
                    return false;
                }

                if (lua_pcall (state, 0, 0, 0) != LUA_OK)
                {
                    problem = said (-1);
                    lua_settop (state, 0);
                    return false;
                }
                return true;
            }

            void tick (const std::vector<Input>& inputs) override
            {
                if (state == nullptr || tickFunction == LUA_NOREF)
                    return;

                lua_rawgeti (state, LUA_REGISTRYINDEX, tickFunction);
                lua_createtable (state, static_cast<int> (inputs.size()), 0);

                lua_Integer at = 1;
                for (const auto& input : inputs)
                {
                    lua_createtable (state, static_cast<int> (input.atoms.size()) + 1, 1);
                    lua_pushlstring (state, input.to.data(), input.to.size());
                    lua_rawseti (state, -2, 1);

                    lua_Integer slot = 2;
                    for (const auto& atom : input.atoms)
                    {
                        if (atom.isNumber)
                            pushNumber (atom.number);
                        else
                            lua_pushlstring (state, atom.word.data(), atom.word.size());
                        lua_rawseti (state, -2, slot++);
                    }
                    lua_pushinteger (state, slot - 1);
                    lua_setfield (state, -2, "n");
                    lua_rawseti (state, -2, at++);
                }

                if (lua_pcall (state, 1, 0, 0) != LUA_OK)
                {
                    output.print (said (-1));
                    lua_settop (state, 0);
                }
            }

            std::vector<std::string> receives() override
            {
                return hearing;
            }

            void interrupt() noexcept override
            {
                stopping.store (true);
            }

        private:
            ScriptOutput& output;
            lua_State* state = nullptr;
            int tickFunction = LUA_NOREF;
            std::atomic<bool> stopping { false };
            std::vector<std::string> hearing;
            std::string preludeProblem;
            std::size_t used = 0;

            static LuaLanguage& of (lua_State* L)
            {
                return **static_cast<LuaLanguage**> (lua_getextraspace (L));
            }

            /*  A whole number goes in as Lua's integer, so a MIDI note prints
                60 and not 60.0; any other as a float. */
            void pushNumber (double value)
            {
                if (std::isfinite (value) && std::abs (value) < 9.0e15)
                {
                    const auto whole = static_cast<lua_Integer> (value);
                    const auto back = static_cast<double> (whole);
                    if (! (back < value) && ! (value < back))
                    {
                        lua_pushinteger (state, whole);
                        return;
                    }
                }
                lua_pushnumber (state, value);
            }

            std::string said (int index)
            {
                if (lua_touserdata (state, index) == &interruptMark)
                    return "interrupted: stuck past the budget";
                return luaProblem (luaL_tolstring (state, index, nullptr));
            }

            static void* allocate (void* self, void* block, std::size_t oldSize, std::size_t newSize)
            {
                auto& language = *static_cast<LuaLanguage*> (self);
                const auto had = block != nullptr ? oldSize : 0;

                if (newSize == 0)
                {
                    std::free (block);
                    language.used -= had;
                    return nullptr;
                }

                if (language.used - had + newSize > memoryCap)
                    return nullptr;

                auto* moved = std::realloc (block, newSize);
                if (moved != nullptr)
                    language.used = language.used - had + newSize;
                return moved;
            }

            static void checkStopped (lua_State* L, lua_Debug*)
            {
                if (of (L).stopping.load())
                {
                    lua_pushlightuserdata (L, &interruptMark);
                    lua_error (L);
                }
            }

            static int doorOut (lua_State* L)
            {
                auto& self = of (L);
                const auto count = lua_gettop (L);

                Sent sent;
                std::size_t length = 0;
                const auto* to = luaL_checklstring (L, 1, &length);
                sent.to.assign (to, length);

                const auto add = [&sent, L] (int index)
                {
                    if (lua_type (L, index) == LUA_TNUMBER)
                    {
                        sent.atoms.push_back (Atom::of (static_cast<double> (lua_tonumber (L, index))));
                    }
                    else if (lua_type (L, index) == LUA_TSTRING)
                    {
                        std::size_t size = 0;
                        const auto* text = lua_tolstring (L, index, &size);
                        sent.atoms.push_back (Atom::of (std::string (text, size)));
                    }
                    else if (lua_type (L, index) == LUA_TBOOLEAN)
                    {
                        sent.atoms.push_back (Atom::of (lua_toboolean (L, index) != 0 ? 1.0 : 0.0));
                    }
                };

                for (int i = 2; i <= count; ++i)
                {
                    if (lua_type (L, i) == LUA_TTABLE)
                    {
                        const auto items = static_cast<int> (luaL_len (L, i));
                        for (int item = 1; item <= items; ++item)
                        {
                            lua_rawgeti (L, i, item);
                            add (lua_gettop (L));
                            lua_pop (L, 1);
                        }
                    }
                    else
                    {
                        add (i);
                    }
                }

                sent.selector = sent.atoms.empty() ? "bang" : "list";
                self.output.send (std::move (sent));
                return 0;
            }

            static int doorPrint (lua_State* L)
            {
                std::size_t length = 0;
                const auto* line = luaL_checklstring (L, 1, &length);
                of (L).output.print (std::string (line, length));
                return 0;
            }

            static int doorHears (lua_State* L)
            {
                auto& self = of (L);
                self.hearing.clear();
                for (int i = 1; i <= lua_gettop (L); ++i)
                {
                    std::size_t length = 0;
                    const auto* address = lua_tolstring (L, i, &length);
                    if (address != nullptr)
                        self.hearing.emplace_back (address, length);
                }
                return 0;
            }
        };
    }

    std::unique_ptr<ScriptLanguage> makeLua (ScriptOutput& output)
    {
        return std::make_unique<LuaLanguage> (output);
    }

    std::string_view luaPrelude()
    {
        return prelude;
    }

    std::string luaProblem (std::string_view message)
    {
        //  "script:7: message" - the chunk's name, its line, then what Lua said.
        const auto colon = message.find (':');
        if (colon != std::string_view::npos)
        {
            auto end = colon + 1;
            while (end < message.size() && std::isdigit (static_cast<unsigned char> (message[end])))
                ++end;
            if (end > colon + 1 && end + 1 < message.size() && message[end] == ':' && message[end + 1] == ' ')
                return std::string (message.substr (end + 2)) + " (line "
                     + std::string (message.substr (colon + 1, end - colon - 1)) + ")";
        }
        return std::string (message);
    }
}
