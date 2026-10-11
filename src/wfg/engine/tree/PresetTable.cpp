// This file is part of Go.dot — https://github.com/pob31/go.dot
//
// Copyright (C) 2026 Pierre-Olivier Boulant
//
// Go.dot is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. Go.dot is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
// (LICENSE, at the repository root) for more details.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <wfg/engine/tree/PresetTable.h>

#include <wfg/engine/json/JsonValue.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <initializer_list>

namespace wfg::tree
{
    namespace
    {
        std::string stringOf (const json::Value& object, const char* key)
        {
            const auto* value = object.find (key);
            return value != nullptr && value->isString() ? value->asString() : std::string {};
        }

        int intOf (const json::Value& object, const char* key, int fallback)
        {
            const auto* value = object.find (key);
            return value != nullptr && value->isNumber() ? static_cast<int> (value->asNumber()) : fallback;
        }

        bool oneOf (const std::string& word, std::initializer_list<const char*> words)
        {
            return std::any_of (words.begin(), words.end(),
                                [&word] (const char* each) { return word == each; });
        }

        /*  WHAT A TRANSPORT CAN CARRY (namespace draft §57, AFJ): a datagram
            takes OSC, or raw MIDI for a desk that reads MIDI Show Control off
            the network; a TCP link takes any of the four; a MIDI port takes
            MIDI bytes and nothing else. */
        bool carries (const std::string& transport, const std::string& wire)
        {
            if (transport == "udp")  return wire == "osc" || wire == "midi";
            if (transport == "tcp")  return oneOf (wire, { "osc", "rcp", "line", "midi" });
            if (transport == "midi") return wire == "midi";
            return false;
        }

        std::string spaced (const std::vector<std::string>& words)
        {
            std::string text;

            for (const auto& word : words)
                text += (text.empty() ? "" : " ") + word;

            return text;
        }
    }

    std::string Preset::rootRow() const
    {
        return spaced (roots);
    }

    Preset readPreset (const std::string& slug, std::string_view jsonText)
    {
        Preset preset;
        preset.slug = slug;
        preset.text = std::string (jsonText);

        /*  The first thing wrong is the whole answer: a file with no GODOT key
            is not a preset, and saying what else it lacks would be noise. */
        auto failed = [&preset] (std::string why)
        {
            preset.problem = std::move (why);
            return preset;
        };

        const auto parsed = json::parse (jsonText);

        if (! parsed.ok())
            return failed (slug + ": not valid JSON at line " + std::to_string (parsed.line) + ": " + parsed.error);

        if (! parsed.value->isObject())
            return failed (slug + ": not a description: the file is not a JSON object");

        const auto* godot = parsed.value->find ("GODOT");

        if (godot == nullptr || ! godot->isObject())
            return failed (slug + ": no GODOT key at the root, so not a preset: a description becomes one"
                                  " with PRESET, VERSION, VENDOR, MODEL and SOURCES there");

        if (const auto named = stringOf (*godot, "PRESET"); named != slug)
            return failed (slug + ": GODOT.PRESET says \"" + named + "\" and the file is named \"" + slug
                                + "\"; a preset has one name");

        preset.vendor = stringOf (*godot, "VENDOR");
        preset.model = stringOf (*godot, "MODEL");

        if (preset.vendor.empty() || preset.model.empty())
            return failed (slug + ": VENDOR and MODEL name the device");

        preset.version = intOf (*godot, "VERSION", 0);

        if (preset.version < 1)
            return failed (slug + ": VERSION is a whole number from 1");

        if (const auto transport = stringOf (*godot, "TRANSPORT"); ! transport.empty())
            preset.transport = transport;

        if (const auto wire = stringOf (*godot, "WIRE"); ! wire.empty())
            preset.wire = wire;

        if (const auto framing = stringOf (*godot, "FRAMING"); ! framing.empty())
            preset.framing = framing;

        preset.readback = stringOf (*godot, "READBACK");

        if (! oneOf (preset.transport, { "udp", "tcp", "midi" }))
            return failed (slug + ": TRANSPORT is udp, tcp or midi, not \"" + preset.transport + "\"");

        if (! oneOf (preset.wire, { "osc", "rcp", "line", "midi" }))
            return failed (slug + ": WIRE is osc, rcp, line or midi, not \"" + preset.wire + "\"");

        if (! oneOf (preset.framing, { "length", "slip" }))
            return failed (slug + ": FRAMING is length or slip, not \"" + preset.framing + "\"");

        if (! preset.readback.empty()
              && ! oneOf (preset.readback, { "oscquery", "notify", "xremote", "subscribe", "get", "midi" }))
            return failed (slug + ": READBACK is oscquery, notify, xremote, subscribe, get or midi, not \"" + preset.readback + "\"");

        if (! carries (preset.transport, preset.wire))
            return failed (slug + ": a " + preset.transport + " transport cannot carry the " + preset.wire + " wire");

        preset.port = intOf (*godot, "PORT", 0);

        if (preset.port < 0 || preset.port > 65535)
            return failed (slug + ": PORT is 0 to 65535");

        /*  PROVENANCE IS NOT OPTIONAL (presets/devices/README.md): a file
            written from memory of a protocol proves nothing, so one that names
            no document is refused rather than shipped. */
        const auto* sources = godot->find ("SOURCES");

        if (sources == nullptr || ! sources->isArray() || sources->asArray().empty())
            return failed (slug + ": SOURCES names nothing; a preset says which documents it was written from");

        for (const auto& source : sources->asArray())
        {
            if (! source.isString() || source.asString().empty())
                return failed (slug + ": every SOURCES entry is a sentence naming a document");

            preset.sources.push_back (source.asString());
        }

        preset.generated = stringOf (*godot, "GENERATED");
        preset.verified = stringOf (*godot, "VERIFIED");

        preset.roots = rootsOfNamespace (jsonText);

        if (preset.roots.empty())
            return failed (slug + ": no root: FULL_PATH names one, or the file is rooted at \"/\" with the roots under CONTENTS");

        /*  THE DESCRIPTION ITSELF, read as a device made from it will read it,
            with the roots the file gives: what refuses here refuses in the
            Network tab, in the same words. */
        MountDeclaration declaration;
        declaration.id = "PRESET00";
        declaration.prefix = preset.rootRow();
        declaration.namespaceFile = slug + ".json";
        declaration.port = preset.port > 0 ? preset.port : 1;

        const auto description = readNamespace (declaration, jsonText);

        if (! description.ok)
            return failed (description.problems.empty() ? slug + ": the description does not load"
                                                        : description.problems.front());

        preset.nodeCount = static_cast<int> (description.nodes.size());

        if (preset.nodeCount == 0)
            return failed (slug + ": the description has no node");

        return preset;
    }

    void PresetTable::scan (const std::string& path)
    {
        presets.clear();
        folderPath = path;

        const auto directory = juce::File::isAbsolutePath (juce::String (path))
                                 ? juce::File (juce::String (path))
                                 : juce::File::getCurrentWorkingDirectory().getChildFile (juce::String (path));

        if (! directory.isDirectory())
            return;

        for (const auto& file : directory.findChildFiles (juce::File::findFiles, false, "*.json"))
            presets.push_back (readPreset (file.getFileNameWithoutExtension().toStdString(),
                                           file.loadFileAsString().toStdString()));

        std::sort (presets.begin(), presets.end(),
                   [] (const Preset& a, const Preset& b) { return a.slug < b.slug; });
    }

    const Preset* PresetTable::find (const std::string& slug) const noexcept
    {
        const auto it = std::find_if (presets.begin(), presets.end(),
                                      [&slug] (const Preset& each) { return each.slug == slug; });
        return it != presets.end() ? &*it : nullptr;
    }
}
