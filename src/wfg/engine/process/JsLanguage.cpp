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

#include <wfg/engine/process/JsLanguage.h>

#include <juce_javascript/juce_javascript.h>

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace wfg::process
{
    namespace
    {
        /*  THE PRELUDE (AMR): `godot`, as namespace draft §66.3 lists it. Run
            once before the script, in a scope of its own; the native door
            (`__host`) is taken into it and removed from the script's sight,
            and every name it leaves the script cannot be written over. A time
            is in seconds and counted in ticks of 20 ms, never read off a clock,
            so a script's timers are the tick's. */
        constexpr const char* prelude = R"js(
(function () {
    'use strict';
    const host = globalThis.__host;
    delete globalThis.__host;

    const hears = new Map();
    const everything = [];
    const midiHears = [];
    const everyTick = [];
    const timers = new Map();
    let nextTimer = 1;
    let ticks = 0;

    function problem (e) {
        const isError = e !== null && typeof e === 'object';
        const message = isError && 'message' in e ? String (e.message) : String (e);
        const at = /:(\d+)/.exec (isError && e.stack ? String (e.stack) : '');
        return at ? message + ' (line ' + at[1] + ')' : message;
    }

    function call (fn, args) {
        try { fn (...args); } catch (e) { host.print (problem (e)); }
    }

    function needFunction (fn, what) {
        if (typeof fn !== 'function')
            throw new TypeError (what + ' needs a function');
    }

    function needAddress (address, what) {
        if (typeof address !== 'string' || address.length === 0)
            throw new TypeError (what + ' needs an address');
    }

    function toTicks (seconds, what) {
        const n = Number (seconds);
        if (! Number.isFinite (n) || n < 0)
            throw new TypeError (what + ' needs a time in seconds');
        return Math.max (1, Math.round (n * 50));
    }

    function timer (seconds, fn, repeat, what) {
        needFunction (fn, what);
        const every = toTicks (seconds, what);
        const id = nextTimer++;
        timers.set (id, { due: ticks + every, every: repeat ? every : 0, fn });
        return id;
    }

    const godot = {
        self: 'self',
        hear (address, fn) {
            needAddress (address, 'godot.hear');
            needFunction (fn, 'godot.hear');
            let list = hears.get (address);
            if (list === undefined) {
                list = [];
                hears.set (address, list);
                host.hears (...hears.keys ());
            }
            list.push (fn);
        },
        hearAll (fn) {
            needFunction (fn, 'godot.hearAll');
            everything.push (fn);
        },
        send (address, ...values) {
            needAddress (address, 'godot.send');
            host.out (address, ...values);
        },
        set (address, ...values) {
            needAddress (address, 'godot.set');
            const row = address.startsWith ('/godot/') ? address : '/godot/' + address.replace (/^\/+/, '');
            host.out (row, ...values);
        },
        command (name, ...args) {
            needAddress (name, 'godot.command');
            host.out ('/godot/cmd/' + name.replace (/^\/+/, '').replace (/\./g, '/'), ...args);
        },
        fire (cue) {
            host.out ('/godot/cmd/cue/fire', cue);
        },
        midi: Object.freeze ({
            hear (fn) {
                needFunction (fn, 'godot.midi.hear');
                midiHears.push (fn);
            },
            send (...bytes) {
                host.out ('#midi', ...bytes);
            }
        }),
        tick (fn) {
            needFunction (fn, 'godot.tick');
            everyTick.push (fn);
        },
        after (seconds, fn) { return timer (seconds, fn, false, 'godot.after'); },
        every (seconds, fn) { return timer (seconds, fn, true, 'godot.every'); },
        cancel (id) { timers.delete (id); }
    };

    function tick (inputs) {
        ++ticks;
        for (const input of inputs) {
            const to = input[0];
            const values = input.slice (1);
            if (to === '#midi') {
                for (const fn of midiHears)
                    call (fn, values);
                continue;
            }
            const list = hears.get (to);
            if (list !== undefined)
                for (const fn of list)
                    call (fn, values);
            for (const fn of everything)
                call (fn, [to, ...values]);
        }
        for (const fn of everyTick)
            call (fn, [ticks]);
        for (const [id, t] of Array.from (timers)) {
            if (t.due > ticks || ! timers.has (id))
                continue;
            if (t.every > 0)
                t.due += t.every;
            else
                timers.delete (id);
            call (t.fn, []);
        }
    }

    const say = (...parts) => host.print (parts.map ((p) => String (p)).join (' '));
    const fixed = (name, value) => Object.defineProperty (globalThis, name,
        { value, writable: false, configurable: false, enumerable: false });

    fixed ('godot', Object.freeze (godot));
    fixed ('print', say);
    fixed ('console', Object.freeze ({ log: say }));
    fixed ('__godot_tick', tick);
}) ();
)js";

        /*  THE NATIVE DOOR the prelude takes: what a script sends, what it
            prints, and the addresses it hears. */
        class Door final : public juce::DynamicObject
        {
        public:
            Door (ScriptOutput& outputToUse, std::vector<std::string>& hearingToUse)
            {
                setMethod ("out", [&output = outputToUse] (const juce::var::NativeFunctionArgs& args)
                {
                    if (args.numArguments < 1)
                        return juce::var();

                    Sent sent;
                    sent.to = args.arguments[0].toString().toStdString();

                    const auto add = [&sent] (const juce::var& value)
                    {
                        if (value.isString())
                            sent.atoms.push_back (Atom::of (value.toString().toStdString()));
                        else if (value.isInt() || value.isInt64() || value.isDouble() || value.isBool())
                            sent.atoms.push_back (Atom::of (static_cast<double> (value)));
                    };

                    for (int i = 1; i < args.numArguments; ++i)
                    {
                        if (const auto* list = args.arguments[i].getArray())
                            for (const auto& value : *list)
                                add (value);
                        else
                            add (args.arguments[i]);
                    }

                    sent.selector = sent.atoms.empty() ? "bang" : "list";
                    output.send (std::move (sent));
                    return juce::var();
                });

                setMethod ("print", [&output = outputToUse] (const juce::var::NativeFunctionArgs& args)
                {
                    std::string line;
                    for (int i = 0; i < args.numArguments; ++i)
                    {
                        if (i > 0)
                            line += ' ';
                        line += args.arguments[i].toString().toStdString();
                    }
                    output.print (std::move (line));
                    return juce::var();
                });

                setMethod ("hears", [&hearing = hearingToUse] (const juce::var::NativeFunctionArgs& args)
                {
                    hearing.clear();
                    for (int i = 0; i < args.numArguments; ++i)
                        hearing.push_back (args.arguments[i].toString().toStdString());
                    return juce::var();
                });
            }
        };

        class JsLanguage final : public ScriptLanguage
        {
        public:
            explicit JsLanguage (ScriptOutput& outputToUse)
                : output (outputToUse)
            {
                /*  A backstop only: the host interrupts a script stuck for a
                    second (AMQ) long before this. */
                engine.maximumExecutionTime = juce::RelativeTime::seconds (60.0);
                engine.registerNativeObject ("__host", new Door (output, hearing));

                const auto result = engine.execute (prelude);
                if (result.failed())
                    preludeProblem = "Go.dot's JavaScript prelude failed: " + result.getErrorMessage().toStdString();
            }

            bool load (const std::string& source, std::string& problem) override
            {
                if (! preludeProblem.empty())
                {
                    problem = preludeProblem;
                    return false;
                }

                const auto result = engine.execute (juce::String::fromUTF8 (source.data(), static_cast<int> (source.size())));
                if (result.failed())
                {
                    problem = javascriptProblem (result.getErrorMessage().toStdString());
                    return false;
                }
                return true;
            }

            void tick (const std::vector<Input>& inputs) override
            {
                juce::Array<juce::var> list;
                list.ensureStorageAllocated (static_cast<int> (inputs.size()));

                for (const auto& input : inputs)
                {
                    juce::Array<juce::var> one;
                    one.add (juce::String::fromUTF8 (input.to.data(), static_cast<int> (input.to.size())));
                    for (const auto& atom : input.atoms)
                        one.add (atom.isNumber ? juce::var (atom.number)
                                               : juce::var (juce::String::fromUTF8 (atom.word.data(), static_cast<int> (atom.word.size()))));
                    list.add (juce::var (one));
                }

                const juce::var arguments[] { juce::var (list) };
                const juce::var::NativeFunctionArgs args (juce::var(), arguments, 1);
                auto result = juce::Result::ok();
                engine.callFunction ("__godot_tick", args, &result);

                if (result.failed())
                    output.print (javascriptProblem (result.getErrorMessage().toStdString()));
            }

            std::vector<std::string> receives() override
            {
                return hearing;
            }

            void interrupt() noexcept override
            {
                engine.stop();
            }

        private:
            ScriptOutput& output;
            std::vector<std::string> hearing;
            std::string preludeProblem;
            juce::JavascriptEngine engine;
        };
    }

    std::unique_ptr<ScriptLanguage> makeJavascript (ScriptOutput& output)
    {
        return std::make_unique<JsLanguage> (output);
    }

    std::string_view javascriptPrelude()
    {
        return prelude;
    }

    std::string javascriptProblem (std::string_view messageAndStack)
    {
        const auto end = messageAndStack.find ('\n');
        std::string message (messageAndStack.substr (0, end));
        while (! message.empty() && (message.back() == ' ' || message.back() == '\r'))
            message.pop_back();

        if (end == std::string_view::npos)
            return message;

        //  The first frame's line: the first ":<digits>" in the stack.
        const auto stack = messageAndStack.substr (end + 1);
        for (std::size_t i = 0; i + 1 < stack.size(); ++i)
        {
            if (stack[i] != ':' || ! std::isdigit (static_cast<unsigned char> (stack[i + 1])))
                continue;

            std::size_t j = i + 1;
            while (j < stack.size() && std::isdigit (static_cast<unsigned char> (stack[j])))
                ++j;
            return message + " (line " + std::string (stack.substr (i + 1, j - i - 1)) + ")";
        }
        return message;
    }
}
