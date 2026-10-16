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

#include <wfg/engine/binding/BindingBridge.h>

#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/command/Command.h>
#include <wfg/engine/command/CommandRegistry.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

namespace wfg::binding
{
    namespace
    {
        constexpr double tickSeconds = 1.0 / static_cast<double> (TickClock::rateHz);

        //  Below this a push or a difference is none.
        constexpr double nothing = 1.0e-12;

        /*  WHAT THE INPUT THREADS HEARD OF ONE BINDING since the last tick: the
            latest place of an absolute or a rate control, the steps of a
            relative one summed. */
        struct Heard
        {
            std::optional<double> input;
            int steps = 0;
        };

        /*  A 14-BIT PAIR'S TWO HALVES AS LAST HEARD: the coarse on the
            binding's number, the fine on the number plus 32. A coarse value
            clears the fine, as MIDI says it does, so a box that sends only the
            coarse half still moves the whole range. */
        struct Pair
        {
            int coarse = 0;
            int fine = 0;
        };

        /*  A RUN THAT HAS STARTED AND NOT FINISHED (AQY): its pre-wait, its
            action, a stop not yet landed, its post-wait. `armed` and
            `preparing` are a run made ready ahead of a GO, which nothing that
            asks whether a cue is running counts (PRD §3.12). */
        bool isGoing (std::string_view state) noexcept
        {
            return state == "waiting" || state == "playing" || state == "stopping" || state == "postWait";
        }

        const std::string& textAt (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            static const std::string none;
            const auto* node = snapshot.find (address);

            if (node == nullptr || node->values.size() != 1 || ! node->values.front().isString())
                return none;

            return node->values.front().getString();
        }

        /*  WHAT A BINDING MOVES, read off the tree: its range and unit, whether
            it is a whole number, and where it is. Only a number one value wide
            that may be written and read; anything else is moved by nothing. */
        struct Target
        {
            TargetRange range;
            std::optional<double> current;
        };

        std::optional<Target> targetOf (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            const auto* node = snapshot.find (address);

            if (node == nullptr || node->access != tree::Access::readWrite || node->typeTags.size() != 1)
                return std::nullopt;

            const auto tag = node->typeTags.front();

            if (tag != 'i' && tag != 'h' && tag != 'f' && tag != 'd')
                return std::nullopt;

            Target target;
            target.range.integer = tag == 'i' || tag == 'h';
            target.range.hasMinimum = node->hasMinimum();
            target.range.minimum = node->minimum();
            target.range.hasMaximum = node->hasMaximum();
            target.range.maximum = node->maximum();
            target.range.unit = node->unit();

            if (const auto value = node->soleValue(); value.has_value() && value->isNumber() && ! value->isNonFinite())
                target.current = value->asDouble();

            return target;
        }

        Event writeOf (const Binding& binding, const std::string& address, double value, bool integer)
        {
            constexpr auto lowest = static_cast<double> (std::numeric_limits<std::int32_t>::min());
            constexpr auto highest = static_cast<double> (std::numeric_limits<std::int32_t>::max());

            Event event;
            event.origin = "binding:" + binding.id;
            event.command = "node.set";
            event.args = { osc::Value::string (address),
                           integer ? osc::Value::int32 (static_cast<std::int32_t> (std::lround (std::clamp (value, lowest, highest))))
                                   : osc::Value::float64 (value) };
            return event;
        }

        /*  WHAT A BINDING IS BOUND TO, as one word: a binding edited to move
            something else, or to move it another way, starts again. */
        std::string shapeOf (const Binding& binding)
        {
            const char* control = binding.control == Control::absolute ? "absolute"
                                : binding.control == Control::relative ? "relative" : "rate";
            return binding.targetAddress() + " " + control + " " + binding.cue;
        }
    }

    //==========================================================================
    struct BindingBridge::State
    {
        //  The input threads' and the tick's, under `inboxGuard`.
        std::mutex inboxGuard;
        std::shared_ptr<const BindingIndex> heardIndex;
        std::map<std::string, Heard> inbox;
        std::map<std::string, Pair> pairs;

        //  The tick thread's alone.
        struct Held
        {
            Pickup pickup;
            double deflection = 0.0;        ///< rate: the push it is held at, minus one to one
            bool heldOver = false;          ///< rate: pushed through a double Esc, not yet back at rest
            std::optional<double> written;  ///< the value it last wrote
            bool live = false;
            std::string shape;
        };

        std::shared_ptr<const BindingIndex> index;
        std::shared_ptr<const tree::TreeSnapshot> snapshot;
        std::map<std::string, Held> held;

        static void arm (Held& held) noexcept
        {
            held.pickup.reset();
            held.deflection = 0.0;
            held.heldOver = false;
            held.written.reset();
        }

        /*  ABSOLUTE: the value for where the control is, once it has picked
            the value up (AQZ). A value moved by something else since this
            binding last wrote it must be picked up again. Picked up on the
            control's travel, so a step means the same along a level's law as
            along a line. */
        std::optional<double> absolute (const Binding& binding, Held& state, double input, const Target& target)
        {
            const auto travel = travelOf (binding.scale, input);
            const auto wanted = absoluteValue (binding.scale, travel, target.range);

            if (binding.pickup && target.current.has_value())
            {
                const auto at = travelFor (binding.scale, *target.current, target.range);

                if (state.pickup.caught && state.written.has_value()
                      && std::abs (travelFor (binding.scale, *state.written, target.range) - at) > pickupStep)
                    state.pickup.caught = false;

                if (! pickUp (state.pickup, travel, at, pickupStep))
                    return std::nullopt;
            }

            return wanted;
        }

        /*  RATE: a new place moves the push; a control held over a double Esc
            moves nothing until it has come back to rest (ARB). Then the push,
            for one tick's time. */
        std::optional<double> rate (const Binding& binding, Held& state, const Heard* heard, const Target& target)
        {
            if (heard != nullptr && heard->input.has_value())
            {
                const auto push = deflectionOf (binding.scale, *heard->input);

                if (! state.heldOver)
                    state.deflection = push;
                else if (std::abs (push) < nothing)
                    state.heldOver = false;
            }

            if (std::abs (state.deflection) < nothing || ! target.current.has_value())
                return std::nullopt;

            return rateValue (binding.scale, *target.current, state.deflection, tickSeconds, target.range);
        }

        /*  WHICH BINDINGS ARE LIVE NOW (AQY): every one switched on with no
            cue; one of a cue while a run of it is going. A binding that comes
            alive or lets go starts again. */
        void refreshLiveness()
        {
            if (index == nullptr)
                return;

            const auto ofCues = std::any_of (index->bindings.begin(), index->bindings.end(),
                                             [] (const Binding& binding) { return ! binding.cue.empty(); });

            std::set<std::string> going;

            if (ofCues && snapshot != nullptr)
                for (const auto* run : snapshot->childrenOf ("/godot/run"))
                    if (run->isContainer() && isGoing (textAt (*snapshot, run->address + "/state")))
                        going.insert (textAt (*snapshot, run->address + "/cue"));

            for (const auto& binding : index->bindings)
            {
                auto& state = held[binding.id];
                const auto live = binding.enabled && (binding.cue.empty() || going.count (binding.cue) > 0);

                if (live != state.live)
                {
                    arm (state);
                    state.live = live;
                }
            }
        }
    };

    //==========================================================================
    BindingBridge::BindingBridge() : impl (std::make_unique<State>()) {}
    BindingBridge::~BindingBridge() = default;

    void BindingBridge::publish (std::shared_ptr<const BindingIndex> index)
    {
        const auto gone = [&index] (const std::string& id)
        {
            return index == nullptr || index->find (id) == nullptr;
        };

        {
            const std::lock_guard<std::mutex> lock { impl->inboxGuard };
            impl->heardIndex = index;

            std::erase_if (impl->inbox, [&gone] (const auto& entry) { return gone (entry.first); });
            std::erase_if (impl->pairs, [&gone] (const auto& entry) { return gone (entry.first); });
        }

        std::erase_if (impl->held, [&gone] (const auto& entry) { return gone (entry.first); });

        if (index != nullptr)
            for (const auto& binding : index->bindings)
            {
                auto& state = impl->held[binding.id];
                auto shape = shapeOf (binding);

                if (shape != state.shape)
                {
                    State::arm (state);
                    state.shape = std::move (shape);
                }
            }

        impl->index = std::move (index);
        impl->refreshLiveness();
    }

    bool BindingBridge::midiArrived (const std::string& portId, const midi::Bytes& message)
    {
        const auto arrival = decodeMidi (message.data(), message.size());

        if (! arrival.has_value())
            return false;

        const std::lock_guard<std::mutex> lock { impl->inboxGuard };

        if (impl->heardIndex == nullptr)
            return false;

        const auto matched = matchMidi (*impl->heardIndex, portId, *arrival);

        for (const auto* binding : matched)
        {
            auto value = arrival->value;
            auto coarseHalf = true;

            if (binding->type == MidiType::controlChange14)
            {
                auto& pair = impl->pairs[binding->id];
                coarseHalf = arrival->number == binding->number;

                if (coarseHalf)
                    pair = { value, 0 };
                else
                    pair.fine = value;

                value = pair.coarse * 128 + pair.fine;
            }

            auto& heard = impl->inbox[binding->id];

            /*  RELATIVE, AS STEPS: a seven-bit value by the usual offset - 1 to
                63 up, 65 to 127 down; a 14-bit pair by its coarse half alone,
                the same way; pitch bend by how far it is from its middle, in
                coarse steps. */
            if (binding->control == Control::relative)
            {
                if (binding->type == MidiType::pitchBend)
                    heard.steps += (value >> 7) - 64;
                else if (binding->type == MidiType::controlChange14)
                    heard.steps += coarseHalf ? midiSteps (value >> 7) : 0;
                else
                    heard.steps += midiSteps (value);
            }
            else
            {
                heard.input = midiFraction (binding->type, value);
            }
        }

        return ! matched.empty();
    }

    bool BindingBridge::oscArrived (const std::string& address, const std::vector<osc::Value>& args)
    {
        const std::lock_guard<std::mutex> lock { impl->inboxGuard };

        if (impl->heardIndex == nullptr)
            return false;

        const auto matched = matchOsc (*impl->heardIndex, address);

        for (const auto* binding : matched)
        {
            const auto at = static_cast<std::size_t> (std::max (binding->argument, 0));

            if (at >= args.size())
                continue;

            //  The argument as it comes: a number, or true and false as one and nought.
            const auto& value = args[at];
            double number = 0.0;

            if (value.isBool())
                number = value.getBool() ? 1.0 : 0.0;
            else if (value.isNumber() && ! value.isNonFinite())
                number = value.asDouble();
            else
                continue;

            auto& heard = impl->inbox[binding->id];

            if (binding->control == Control::relative)
                heard.steps += static_cast<int> (std::lround (std::clamp (number, -1000.0, 1000.0)));
            else
                heard.input = number;
        }

        return ! matched.empty();
    }

    void BindingBridge::beforeTick (const Submit& submit)
    {
        std::map<std::string, Heard> heard;

        {
            const std::lock_guard<std::mutex> lock { impl->inboxGuard };
            heard.swap (impl->inbox);
        }

        if (impl->index == nullptr || impl->snapshot == nullptr)
            return;

        for (const auto& binding : impl->index->bindings)
        {
            const auto found = heard.find (binding.id);
            const auto* now = found != heard.end() ? &found->second : nullptr;
            auto& state = impl->held[binding.id];

            if (! state.live)
                continue;

            //  Nothing heard moves nothing, but a rate control pushed and held.
            if (now == nullptr && (binding.control != Control::rate || std::abs (state.deflection) < nothing))
                continue;

            const auto address = binding.targetAddress();

            if (address.empty())
                continue;

            const auto target = targetOf (*impl->snapshot, address);

            if (! target.has_value())
                continue;

            std::optional<double> next;

            switch (binding.control)
            {
                case Control::absolute:
                    if (now->input.has_value())
                        next = impl->absolute (binding, state, *now->input, *target);
                    break;

                case Control::relative:
                    if (now->steps != 0 && target->current.has_value())
                        next = relativeValue (binding.scale, *target->current, now->steps, target->range);
                    break;

                case Control::rate:
                    next = impl->rate (binding, state, now, *target);
                    break;
            }

            if (! next.has_value())
                continue;

            //  Where it already is, or held against an end: nothing to write.
            if (target->current.has_value() && std::abs (*next - *target->current) < 1.0e-9)
                continue;

            submit (writeOf (binding, address, *next, target->range.integer));
            state.written = *next;
        }
    }

    void BindingBridge::afterTick (std::shared_ptr<const tree::TreeSnapshot> snapshotToKeep)
    {
        impl->snapshot = std::move (snapshotToKeep);
        impl->refreshLiveness();
    }

    void BindingBridge::letGo()
    {
        //  What was heard and not yet written is an action the press drops.
        {
            const std::lock_guard<std::mutex> lock { impl->inboxGuard };
            impl->inbox.clear();
        }

        for (auto& entry : impl->held)
        {
            auto& state = entry.second;

            if (std::abs (state.deflection) >= nothing)
                state.heldOver = true;

            state.deflection = 0.0;
        }
    }

    //==========================================================================
    void letGoOnDoubleEsc (CommandRegistry& registry, BindingBridge& bridge)
    {
        const auto* plain = registry.find ("run.killAll");

        if (plain == nullptr)
            return;

        auto wrapped = *plain;
        wrapped.handler = [&bridge, before = plain->handler] (CommandContext& context, const std::vector<osc::Value>& args)
        {
            auto outcome = before (context, args);

            if (outcome.applied)
                bridge.letGo();

            return outcome;
        };

        registry.add (std::move (wrapped));
    }
}
