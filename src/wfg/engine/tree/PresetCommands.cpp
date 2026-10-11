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

#include <wfg/engine/tree/PresetCommands.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/tree/TreeCommands.h>

#include <cstdlib>
#include <utility>
#include <vector>

namespace wfg::tree
{
    namespace
    {
        /*  THE FILE INTO THE BUNDLE, ATOMICALLY, as MountFetcher writes a
            fetched description: a temporary beside the target, moved over it,
            so a reader never sees half a file. Nothing is written when the
            file is already there - a second device from the same preset, or a
            replay whose bundle kept it. */
        bool keepInBundle (const juce::File& bundleFolder, const std::string& file, const std::string& text)
        {
            const auto target = bundleFolder.getChildFile (juce::String (file));

            if (target.existsAsFile())
                return true;

            juce::TemporaryFile temporary (target);

            return bundleFolder.getChildFile ("namespaces").createDirectory()
                && temporary.getFile().replaceWithText (juce::String::fromUTF8 (text.data(), static_cast<int> (text.size())))
                && temporary.overwriteTargetFileWithTemporary();
        }

        std::string attributeOf (const doc::ShowDocument& document, const std::string& address)
        {
            return document.getAttribute (address).value_or (std::string {});
        }

        /*  `<slug>@<version>`, as `mount/preset` spells where a device came from. */
        struct FromPreset
        {
            std::string slug;
            int version = 0;
        };

        FromPreset fromPresetOf (const std::string& text)
        {
            const auto at = text.find ('@');

            if (at == std::string::npos)
                return {};

            return { text.substr (0, at), std::atoi (text.c_str() + at + 1) };
        }

        Outcome writeRows (doc::ShowDocument& document, const std::string& mountId,
                           const std::vector<std::pair<std::string, std::string>>& writes)
        {
            const auto base = "/godot/mount/" + mountId + "/";

            for (const auto& [row, text] : writes)
                if (const auto edit = document.setAttribute (base + row, text); ! edit.ok)
                    return Outcome::rejected (edit.reason);

            return Outcome::ok ({});
        }
    }

    std::string presetFileFor (const std::string& slug, int version)
    {
        return "namespaces/" + slug + "-v" + std::to_string (version) + ".json";
    }

    void registerPresetCommands (CommandRegistry& registry, doc::ShowDocument& document, MountTable& mounts,
                                 const juce::File& bundleFolder, const PresetTable* presets)
    {
        registry.add ({ "mount.createFromPreset",
                        "Makes a device from an installed preset: the preset copied into the bundle, the device"
                        " made at the preset's roots with its port and transport, and loaded. The trailing"
                        " arguments are what it drew and decided, on the record for a replay.",
                        { { "slug", 's', false }, { "id", 's', true }, { "prefix", 's', true },
                          { "namespace", 's', true }, { "port", 'i', true }, { "transport", 's', true },
                          { "version", 'i', true }, { "framing", 's', true }, { "wire", 's', true },
                          { "readback", 's', true } },
                        true,
                        [&document, &mounts, &bundleFolder, presets] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto slug = args[0].getString();
                            const auto wantedId = args.size() > 1 ? args[1].getString() : std::string {};

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            const auto* preset = presets != nullptr ? presets->find (slug) : nullptr;

                            /*  FROM THE RECORD WHEN IT HAS ONE, else from the
                                installed preset: a replay has no table and its
                                record says everything the live command decided. */
                            std::string prefixRow, file, transport, framing = "length", wire = "osc", readback;
                            int port = 0;
                            int version = 0;

                            if (args.size() >= 7)
                            {
                                prefixRow = args[2].getString();
                                file = args[3].getString();
                                port = args[4].getInt32();
                                transport = args[5].getString();
                                version = args[6].getInt32();

                                if (args.size() >= 8)
                                    framing = args[7].getString();

                                if (args.size() >= 9)
                                    wire = args[8].getString();

                                if (args.size() >= 10)
                                    readback = args[9].getString();
                            }
                            else if (preset != nullptr && preset->usable())
                            {
                                prefixRow = preset->rootRow();
                                version = preset->version;
                                file = presetFileFor (slug, version);
                                port = preset->port;
                                transport = preset->transport;
                                framing = preset->framing;
                                wire = preset->wire;
                                readback = preset->readback;
                            }
                            else
                            {
                                return Outcome::rejected (preset == nullptr ? reason::unknownPreset
                                                                            : reason::badNamespace);
                            }

                            if (! bundleFolder.getChildFile (juce::String (file)).existsAsFile())
                            {
                                if (preset == nullptr)
                                    return Outcome::rejected (reason::unknownPreset);

                                if (! keepInBundle (bundleFolder, file, preset->text))
                                    return Outcome::rejected (reason::writeFailed);
                            }

                            const auto made = document.createMount (prefixRow, file, wantedId);

                            if (! made.ok)
                                return Outcome::rejected (made.reason);

                            std::vector<std::pair<std::string, std::string>> writes {
                                { "preset", slug + "@" + std::to_string (version) } };

                            if (port > 0)
                                writes.push_back ({ "port", std::to_string (port) });

                            if (transport != "udp")
                                writes.push_back ({ "transport", transport });

                            /*  AND HOW ITS STREAM IS CUT, for a device over a
                                connection (DP.6): what the preset's file says,
                                which an Eos preset says is length. */
                            if (transport == "tcp")
                                writes.push_back ({ "framing", framing });

                            //  And what the bytes are, where they are not OSC (DP.7).
                            if (wire != "osc")
                                writes.push_back ({ "wire", wire });

                            //  And how the device is heard back, where the file says (DP.10).
                            if (! readback.empty())
                                writes.push_back ({ "readback", readback });

                            if (const auto written = writeRows (document, made.id, writes); ! written.applied)
                                return written;

                            /*  LOADED ON THIS TICK, as a fetched description is
                                (mount.described): what fails to load is on the
                                device's problem cell, not a refusal of the make. */
                            loadMountFromBundle (document, mounts, bundleFolder, made.id);

                            return Outcome::ok ({ osc::Value::string (slug), osc::Value::string (made.id),
                                                  osc::Value::string (prefixRow), osc::Value::string (file),
                                                  osc::Value::int32 (port), osc::Value::string (transport),
                                                  osc::Value::int32 (version), osc::Value::string (framing),
                                                  osc::Value::string (wire), osc::Value::string (readback) });
                        } });

        registry.add ({ "mount.refreshPreset",
                        "Points a device made from a preset at the newer version installed: the file written"
                        " beside the old, the namespace, prefix and preset rows moved, the device reloaded."
                        " Refused when nothing newer is installed.",
                        { { "id", 's', false }, { "namespace", 's', true }, { "prefix", 's', true },
                          { "version", 'i', true } },
                        true,
                        [&document, &mounts, &bundleFolder, presets] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto mountId = args[0].getString();

                            if (! mountDeclarationFor (document, mountId))
                                return Outcome::rejected (reason::unknownId);

                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            const auto from = fromPresetOf (attributeOf (document, "/godot/mount/" + mountId + "/preset"));

                            if (from.slug.empty())
                                return Outcome::rejected (reason::badValue);

                            const auto* preset = presets != nullptr ? presets->find (from.slug) : nullptr;

                            std::string file, prefixRow;
                            int version = 0;

                            if (args.size() >= 4)
                            {
                                file = args[1].getString();
                                prefixRow = args[2].getString();
                                version = args[3].getInt32();
                            }
                            else if (preset != nullptr && preset->usable())
                            {
                                if (preset->version <= from.version)
                                    return Outcome::rejected (reason::badValue);

                                version = preset->version;
                                file = presetFileFor (from.slug, version);
                                prefixRow = preset->rootRow();
                            }
                            else
                            {
                                return Outcome::rejected (preset == nullptr ? reason::unknownPreset
                                                                            : reason::badNamespace);
                            }

                            if (! bundleFolder.getChildFile (juce::String (file)).existsAsFile())
                            {
                                if (preset == nullptr)
                                    return Outcome::rejected (reason::unknownPreset);

                                if (! keepInBundle (bundleFolder, file, preset->text))
                                    return Outcome::rejected (reason::writeFailed);
                            }

                            std::vector<std::pair<std::string, std::string>> writes {
                                { "namespace", file }, { "preset", from.slug + "@" + std::to_string (version) } };

                            if (attributeOf (document, "/godot/mount/" + mountId + "/prefix") != prefixRow)
                                writes.push_back ({ "prefix", prefixRow });

                            if (const auto written = writeRows (document, mountId, writes); ! written.applied)
                                return written;

                            loadMountFromBundle (document, mounts, bundleFolder, mountId);

                            return Outcome::ok ({ osc::Value::string (mountId), osc::Value::string (file),
                                                  osc::Value::string (prefixRow), osc::Value::int32 (version) });
                        } });
    }
}
