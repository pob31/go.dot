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

#include <wfg/engine/tree/PresetScanner.h>

#include <wfg/engine/json/JsonValue.h>
#include <wfg/engine/json/JsonWrite.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <utility>

namespace wfg::tree
{
    namespace
    {
        json::Value text (const std::string& words) { return json::Value (words); }
        json::Value number (int value) { return json::Value (static_cast<double> (value)); }

        std::string stringIn (const json::Object& fields, const char* key)
        {
            const auto found = fields.find (key);
            return found != fields.end() && found->second.isString() ? found->second.asString() : std::string {};
        }
    }

    std::string savePresetNow (const PresetSave& save, const std::string& folderPath, std::string& savedAs)
    {
        const juce::File source (juce::String::fromUTF8 (save.source.c_str()));
        juce::MemoryBlock bytes;

        if (! source.loadFileAsData (bytes))
            return "could not read " + save.source;

        const std::string description (static_cast<const char*> (bytes.getData()), bytes.getSize());

        //  THE FILE'S OWN HEADER, when it has one: a copy of a shipped preset keeps what it says.
        json::Object fields;

        if (const auto span = rootGodotSpan (description))
            if (const auto parsed = json::parse (std::string_view (description).substr (span->begin, span->end - span->begin));
                parsed.ok() && parsed.value->isObject())
                fields = parsed.value->asObject();

        const auto ownVersion = stringIn (fields, "PRESET") == save.slug && fields.count ("VERSION") > 0
                                  ? fields.at ("VERSION").asInt() : 0;
        const auto highest = std::max (save.knownVersion, ownVersion);
        const auto version = highest > 0 ? highest + 1 : 1;

        const auto vendor = ! save.vendor.empty() ? save.vendor
                          : ! stringIn (fields, "VENDOR").empty() ? stringIn (fields, "VENDOR") : save.deviceName;
        const auto model = ! save.model.empty() ? save.model
                         : ! stringIn (fields, "MODEL").empty() ? stringIn (fields, "MODEL")
                         : save.deviceName + " (OSCQuery)";

        fields["PRESET"] = text (save.slug);
        fields["VERSION"] = number (version);
        fields["VENDOR"] = text (vendor);
        fields["MODEL"] = text (model);
        fields["TRANSPORT"] = text (save.transport);
        fields["WIRE"] = text (save.wire);

        if (save.transport == "tcp")
            fields["FRAMING"] = text (save.framing);
        else
            fields.erase ("FRAMING");

        if (save.port > 0)
            fields["PORT"] = number (save.port);
        else
            fields.erase ("PORT");

        if (save.queryPort > 0)
            fields["QUERY_PORT"] = number (save.queryPort);
        else
            fields.erase ("QUERY_PORT");

        if (! save.readback.empty() && save.readback != "none")
            fields["READBACK"] = text (save.readback);
        else
            fields.erase ("READBACK");

        /*  WHERE IT CAME FROM, one line more: a capture of a device, or a copy
            of a preset. No script wrote this copy, so GENERATED goes. */
        json::Array sources;

        if (const auto found = fields.find ("SOURCES"); found != fields.end() && found->second.isArray())
            sources = found->second.asArray();

        sources.push_back (text (! save.fromPreset.empty()
                                   ? "copied from " + save.fromPreset + " on " + save.date
                                   : "captured from " + save.deviceName + " at " + save.host + ":"
                                       + std::to_string (save.queryPort) + " on " + save.date));
        fields["SOURCES"] = json::Value (std::move (sources));
        fields.erase ("GENERATED");

        const auto written = json::compact (withGodotHeader (description, json::Value (std::move (fields))));

        if (written.empty())
            return "the description of " + save.deviceName + " is not a JSON object";

        //  READ AS A PRESET BEFORE IT IS ONE: a file that would not be offered is not written.
        if (const auto preset = readPresetQuickly (save.slug, written); ! preset.usable())
            return preset.problem;

        const auto folder = juce::File (juce::String::fromUTF8 (folderPath.c_str()));
        const auto target = folder.getChildFile (juce::String (save.slug) + ".json");

        if (! folder.createDirectory())
            return "could not make the folder " + folderPath;

        juce::TemporaryFile temporary (target);

        if (! temporary.getFile().replaceWithData (written.data(), written.size())
            || ! temporary.overwriteTargetFileWithTemporary())
            return "could not write " + target.getFullPathName().toStdString();

        savedAs = save.slug + "@" + std::to_string (version);
        return {};
    }

    PresetScanner::~PresetScanner()
    {
        stop();
    }

    bool PresetScanner::start()
    {
        if (running.load (std::memory_order_relaxed))
            return false;

        stopping.store (false, std::memory_order_relaxed);
        running.store (true, std::memory_order_relaxed);

        thread = std::thread ([this] { run(); });
        return true;
    }

    void PresetScanner::stop()
    {
        if (! running.load (std::memory_order_relaxed))
            return;

        //  Under the lock, so the thread cannot miss the wake between its test and its wait.
        {
            const std::lock_guard<std::mutex> lock { guard };
            stopping.store (true, std::memory_order_relaxed);
        }

        wake.notify_all();

        if (thread.joinable())
            thread.join();

        running.store (false, std::memory_order_relaxed);
    }

    void PresetScanner::rescan()
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            rescanWanted = true;
        }

        wake.notify_one();
    }

    void PresetScanner::save (PresetSave job)
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            saves.push_back (std::move (job));
        }

        wake.notify_one();
    }

    std::optional<std::vector<Preset>> PresetScanner::take()
    {
        if (! ready.load (std::memory_order_acquire))
            return std::nullopt;

        const std::lock_guard<std::mutex> lock { guard };
        ready.store (false, std::memory_order_relaxed);
        return std::exchange (finished, std::nullopt);
    }

    PresetScanner::Status PresetScanner::status() const
    {
        const std::lock_guard<std::mutex> lock { guard };
        return current;
    }

    void PresetScanner::setStatus (Status now)
    {
        {
            const std::lock_guard<std::mutex> lock { guard };
            current = std::move (now);
        }

        statusStamp.fetch_add (1, std::memory_order_acq_rel);
    }

    void PresetScanner::run()
    {
        for (;;)
        {
            std::vector<PresetSave> toSave;

            {
                std::unique_lock<std::mutex> lock { guard };

                wake.wait (lock, [this] { return stopping.load (std::memory_order_relaxed) || rescanWanted || ! saves.empty(); });

                if (stopping.load (std::memory_order_relaxed))
                    return;

                rescanWanted = false;
                toSave.swap (saves);
            }

            //  THE SAVES FIRST, then the reading that offers them.
            for (const auto& job : toSave)
            {
                std::string savedAs;
                auto problem = savePresetNow (job, yoursPath, savedAs);
                auto now = status();

                now.problem = std::move (problem);

                if (! savedAs.empty())
                    now.saved = std::move (savedAs);

                setStatus (std::move (now));
            }

            /*  Outside the lock: this is the part that touches the disk. The
                quick read, as serve's start reads, so the rows say the same
                thing whichever read made them. */
            auto read = readPresetFolder (yoursPath, true, "yours");

            {
                const std::lock_guard<std::mutex> lock { guard };
                finished = std::move (read);
            }

            ready.store (true, std::memory_order_release);
        }
    }
}
