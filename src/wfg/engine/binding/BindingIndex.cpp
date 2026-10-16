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

#include <wfg/engine/binding/BindingIndex.h>

#include <wfg/engine/document/ShowDocument.h>
#include <juce_data_structures/juce_data_structures.h>
#include <wfg/engine/osc/OscValue.h>

#include <algorithm>

namespace wfg::binding
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        /*  A ROW READ OFF THE ELEMENT, not through `getAttribute`: the index is
            built again after every tick that changed the show, and a bound
            fader under no lock changes it every tick it moves. What the
            document holds is the canonical text; what it leaves out is the
            table's default, which the callers below restate. */
        std::string rowOf (const juce::ValueTree& node, const char* row)
        {
            return node.getProperty (juce::Identifier (row)).toString().toStdString();
        }

        int integerOf (const juce::ValueTree& node, const char* row, int otherwise)
        {
            const auto number = osc::parseDouble (rowOf (node, row));
            return number.has_value() ? static_cast<int> (*number) : otherwise;
        }

        double numberOf (const juce::ValueTree& node, const char* row, double otherwise)
        {
            return osc::parseDouble (rowOf (node, row)).value_or (otherwise);
        }

        bool flagOf (const juce::ValueTree& node, const char* row, bool otherwise)
        {
            const auto text = rowOf (node, row);
            return text.empty() ? otherwise : text == "true";
        }

        MidiType typeOf (const std::string& word) noexcept
        {
            if (word == "controlChange14")  return MidiType::controlChange14;
            if (word == "noteVelocity")     return MidiType::noteVelocity;
            if (word == "pitchBend")        return MidiType::pitchBend;
            if (word == "channelPressure")  return MidiType::channelPressure;
            return MidiType::controlChange;
        }

        Control controlOf (const std::string& word) noexcept
        {
            if (word == "relative") return Control::relative;
            if (word == "rate")     return Control::rate;
            return Control::absolute;
        }
    }

    std::string Binding::targetAddress() const
    {
        if (target.empty())
            return {};

        if (target.front() == '/')
            return target;

        /*  A ROW OF ITS CUE BY NAME (AQY): only a binding of a cue has one to
            name, and only a plain word names a row. */
        if (cue.empty() || target.find ('/') != std::string::npos)
            return {};

        return "/godot/cue/" + cue + "/" + target;
    }

    std::shared_ptr<const BindingIndex> BindingIndex::build (const doc::ShowDocument& document)
    {
        auto index = std::make_shared<BindingIndex>();
        const auto container = document.root().getChildWithName ("Bindings");

        for (const auto& node : container)
        {
            if (node.getType().toString() != "Binding")
                continue;

            Binding binding;
            binding.id = node[idProperty].toString().toStdString();

            if (binding.id.empty())
                continue;

            //  The defaults are the table's (docs/parameters/godot-parameters.csv, `binding`).
            binding.name = rowOf (node, "name");
            binding.cue = rowOf (node, "cue");
            binding.enabled = flagOf (node, "enabled", true);
            binding.source = rowOf (node, "source") == "osc" ? Source::osc : Source::midi;
            binding.port = rowOf (node, "port");
            binding.channel = integerOf (node, "channel", 0);
            binding.type = typeOf (rowOf (node, "type"));
            binding.number = integerOf (node, "number", 0);
            binding.address = rowOf (node, "address");
            binding.argument = integerOf (node, "argument", 0);
            binding.control = controlOf (rowOf (node, "control"));
            binding.target = rowOf (node, "target");
            binding.scale.inLow = numberOf (node, "inLow", 0.0);
            binding.scale.inHigh = numberOf (node, "inHigh", 1.0);
            binding.scale.ownRange = flagOf (node, "ownRange", true);
            binding.scale.outLow = numberOf (node, "outLow", 0.0);
            binding.scale.outHigh = numberOf (node, "outHigh", 1.0);
            binding.scale.invert = flagOf (node, "invert", false);
            binding.scale.speed = numberOf (node, "speed", 0.5);
            binding.scale.deadZone = numberOf (node, "deadZone", 0.1);
            binding.pickup = flagOf (node, "pickup", true);
            binding.feedback = flagOf (node, "feedback", false);

            index->bindings.push_back (std::move (binding));
        }

        return index;
    }

    const Binding* BindingIndex::find (const std::string& id) const
    {
        for (const auto& binding : bindings)
            if (binding.id == id)
                return &binding;

        return nullptr;
    }

    std::optional<MidiArrival> decodeMidi (const std::uint8_t* bytes, std::size_t size) noexcept
    {
        if (bytes == nullptr || size < 2)
            return std::nullopt;

        const auto status = bytes[0] & 0xF0;
        MidiArrival arrival;
        arrival.channel = (bytes[0] & 0x0F) + 1;

        switch (status)
        {
            case 0x90:
                if (size < 3 || bytes[2] == 0)
                    return std::nullopt;     //  a note-on of velocity nought is a note-off

                arrival.kind = MidiArrival::Kind::noteOn;
                arrival.number = bytes[1] & 0x7F;
                arrival.value = bytes[2] & 0x7F;
                return arrival;

            case 0xB0:
                if (size < 3)
                    return std::nullopt;

                arrival.kind = MidiArrival::Kind::controlChange;
                arrival.number = bytes[1] & 0x7F;
                arrival.value = bytes[2] & 0x7F;
                return arrival;

            case 0xD0:
                arrival.kind = MidiArrival::Kind::channelPressure;
                arrival.value = bytes[1] & 0x7F;
                return arrival;

            case 0xE0:
                if (size < 3)
                    return std::nullopt;

                arrival.kind = MidiArrival::Kind::pitchBend;
                arrival.value = (bytes[1] & 0x7F) | ((bytes[2] & 0x7F) << 7);
                return arrival;

            default:
                return std::nullopt;
        }
    }

    std::vector<const Binding*> matchMidi (const BindingIndex& index, const std::string& port,
                                           const MidiArrival& arrival)
    {
        std::vector<const Binding*> out;

        for (const auto& binding : index.bindings)
        {
            if (! binding.enabled || binding.source != Source::midi)
                continue;

            if (! binding.port.empty() && binding.port != port)
                continue;

            if (binding.channel != 0 && binding.channel != arrival.channel)
                continue;

            auto fits = false;

            switch (binding.type)
            {
                case MidiType::controlChange:
                    fits = arrival.kind == MidiArrival::Kind::controlChange && arrival.number == binding.number;
                    break;

                case MidiType::controlChange14:
                    fits = arrival.kind == MidiArrival::Kind::controlChange
                             && (arrival.number == binding.number || arrival.number == binding.number + 32);
                    break;

                case MidiType::noteVelocity:
                    fits = arrival.kind == MidiArrival::Kind::noteOn && arrival.number == binding.number;
                    break;

                case MidiType::pitchBend:
                    fits = arrival.kind == MidiArrival::Kind::pitchBend;
                    break;

                case MidiType::channelPressure:
                    fits = arrival.kind == MidiArrival::Kind::channelPressure;
                    break;
            }

            if (fits)
                out.push_back (&binding);
        }

        return out;
    }

    std::vector<const Binding*> matchOsc (const BindingIndex& index, const std::string& address)
    {
        std::vector<const Binding*> out;

        for (const auto& binding : index.bindings)
            if (binding.enabled && binding.source == Source::osc && ! binding.address.empty()
                  && binding.address == address)
                out.push_back (&binding);

        return out;
    }

    double midiFraction (MidiType type, int value) noexcept
    {
        const auto wide = type == MidiType::controlChange14 || type == MidiType::pitchBend;
        const auto top = wide ? 16383.0 : 127.0;

        return std::clamp (static_cast<double> (value) / top, 0.0, 1.0);
    }
}
