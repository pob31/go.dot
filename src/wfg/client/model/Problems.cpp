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

#include <wfg/client/model/Problems.h>
#include <wfg/client/model/Devices.h>
#include <wfg/client/model/Fx.h>
#include <wfg/client/model/InputList.h>
#include <wfg/client/model/MidiPorts.h>
#include <wfg/client/model/OutputList.h>
#include <wfg/client/model/Rack.h>
#include <wfg/client/model/SerialPorts.h>
#include <wfg/client/model/SettingsPages.h>
#include <wfg/client/model/Surfaces.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/model/Video.h>
#include <wfg/engine/cue/GroupMode.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <utility>

namespace wfg::client::model
{
    namespace
    {
        namespace pageName = settingsPages;

        bool yes (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            return isYes (flag (snapshot, address));
        }

        int integer (const tree::TreeSnapshot& snapshot, const std::string& address)
        {
            return std::atoi (text (snapshot, address).c_str());
        }

        /*  THE ENGINE'S SENTENCE, without the full stop a sentence of its own
            may end with: it is followed here by where it is put right. */
        std::string clause (std::string words)
        {
            while (! words.empty() && (words.back() == '.' || words.back() == ' '))
                words.pop_back();

            return words;
        }

        void add (std::vector<Problem>& out, const char* page, std::string key, std::string word, const std::string& sentence)
        {
            out.push_back ({ page, std::move (key), std::move (word),
                             clause (sentence) + " - Show settings, " + page + "." });
        }

        void addOnce (std::vector<std::string>& keys, std::string key)
        {
            if (! key.empty() && std::find (keys.begin(), keys.end(), key) == keys.end())
                keys.push_back (std::move (key));
        }

        /*  WHAT ONE CUE NEEDS, by the keys `machineProblems` names things by,
            read by address: its kind decides which of its rows name a thing
            that can go missing. */
        std::vector<std::string> needsOfCue (const tree::TreeSnapshot& snapshot, const std::string& id,
                                             const std::string& kind, const std::string& mode,
                                             const ObjectIndex& index, const std::vector<DeviceRow>& devices,
                                             CueNeeds& needs)
        {
            const auto base = "/godot/cue/" + id + "/";
            std::vector<std::string> keys;

            const auto named = [&snapshot, &base, &keys] (const char* row, const char* prefix)
            {
                if (const auto target = text (snapshot, base + row); ! target.empty())
                    addOnce (keys, prefix + target);
            };

            //  The plugins its FX switches in, each by the set's identifier.
            const auto inserts = [&snapshot, &base, &keys]
            {
                for (const auto& fxId : words (text (snapshot, base + "fx")))
                    if (const auto plugin = text (snapshot, "/godot/fx/" + fxId + "/plugin"); ! plugin.empty())
                        addOnce (keys, "plugin:" + plugin);
            };

            const auto file = [&snapshot, &base, &keys, &needs, &id]
            {
                if (const auto name = text (snapshot, base + "file"); ! name.empty())
                {
                    addOnce (keys, "file:" + id);
                    needs.files.emplace_back (id, name);
                }
            };

            if (kind == "media")
            {
                addOnce (keys, "audio");
                inserts();
                file();
            }
            else if (kind == "mic")
            {
                addOnce (keys, "audio");
                named ("input", "input:");

                //  The rack channel it plays through, and that channel's plugins.
                if (const auto channel = text (snapshot, base + "channel"); ! channel.empty())
                    for (const auto& plugin : words (text (snapshot, "/godot/slot/" + channel + "/plugins")))
                        addOnce (keys, "plugin:" + plugin);

                inserts();
            }
            else if (kind == "video")
            {
                addOnce (keys, "renderer");

                //  Every output its canvas is shown on, as an output's own canvas or a zone's.
                if (const auto canvas = text (snapshot, base + "canvas"); ! canvas.empty())
                    if (const auto found = index.outputsOfCanvas.find (canvas); found != index.outputsOfCanvas.end())
                        for (const auto& output : found->second)
                            addOnce (keys, "videoOutput:" + output);

                named ("videoInput", "videoInput:");
                named ("videoInsert", "videoInsert:");
                file();
            }
            else if (kind == "midi")
            {
                named ("port", "port:");
            }
            else if (kind == "process" || kind == "script")
            {
                named ("midiIn", "port:");
                named ("midiOut", "port:");
            }
            else if (kind == "osc")
            {
                if (const auto device = deviceOf (text (snapshot, base + "address"), devices); ! device.empty())
                    addOnce (keys, "device:" + device);
            }
            else if (kind == "timecode")
            {
                //  MTC leaves on the show's port; LTC is drawn on an output, and so needs the audio.
                if (text (snapshot, base + "format") == "ltc")
                    addOnce (keys, "audio");
                else if (const auto port = text (snapshot, "/godot/sync/mtcOut"); ! port.empty())
                    addOnce (keys, "port:" + port);
            }
            else if (kind == "group" && cue::isTimeGroup (cue::GroupMode { mode, {} }))
            {
                //  What a time group follows: the input LTC comes in on, or the port MTC does.
                const auto source = text (snapshot, base + "timeSource");

                if (source == "ltc" || source.empty())
                {
                    if (const auto input = text (snapshot, "/godot/sync/ltcIn"); ! input.empty())
                        addOnce (keys, "input:" + input);
                }
                else if (source == "mtc")
                {
                    if (const auto port = text (snapshot, "/godot/sync/mtcIn"); ! port.empty())
                        addOnce (keys, "port:" + port);
                }
            }

            return keys;
        }

        //  A cue's members in every section - header, members, footer - by identifier.
        std::vector<std::string> membersOf (const tree::TreeSnapshot& snapshot, const std::string& groupId)
        {
            std::vector<std::string> out;
            const auto base = "/godot/cue/" + groupId + "/";

            for (const auto* row : { "headerOrder", "order", "footerOrder" })
                for (auto& id : words (text (snapshot, base + row)))
                    out.push_back (std::move (id));

            return out;
        }
    }

    //==============================================================================
    ObjectIndex indexObjects (const tree::TreeSnapshot& snapshot)
    {
        ObjectIndex out;
        out.revision = text (snapshot, "/godot/document/revision");

        for (const auto& row : readPorts (snapshot))
            out.ports.push_back ({ row.id, row.name, true });

        for (const auto& row : readSerialPorts (snapshot))
            out.serials.push_back ({ row.id, row.name, true });

        for (const auto& row : readInputs (snapshot))
            out.inputs.push_back ({ row.id, row.name, true });

        for (const auto& row : readDevices (snapshot))
            out.devices.push_back ({ row.id, row.name, true });

        for (const auto& row : readSurfaces (snapshot))
            out.surfaces.push_back ({ row.id, row.name, row.enabled });

        for (const auto& row : readVideoOutputs (snapshot))
        {
            out.videoOutputs.push_back ({ row.id, row.label(), row.enabled });

            const auto shows = [&out, &row] (const std::string& canvas)
            {
                if (canvas.empty())
                    return;

                auto& outputs = out.outputsOfCanvas[canvas];

                if (std::find (outputs.begin(), outputs.end(), row.id) == outputs.end())
                    outputs.push_back (row.id);
            };

            shows (row.canvas);

            for (const auto& zone : readZones (snapshot, row.id))
                shows (zone.canvas);
        }

        for (const auto& row : readVideoInputs (snapshot))
            out.videoInputs.push_back ({ row.id, row.label(), row.enabled });

        for (const auto& row : readVideoInserts (snapshot))
            out.videoInserts.push_back ({ row.id, row.label(), true });

        for (const auto& row : readPluginSet (snapshot))
            out.setPlugins.push_back ({ row.id, row.name.empty() ? row.id : row.name, true });

        for (const auto& channel : readRack (snapshot).channels)
        {
            std::vector<ObjectIndex::Named> chain;

            for (const auto& plugin : channel.chain)
                chain.push_back ({ plugin.id, plugin.name.empty() ? plugin.id : plugin.name, true });

            out.rackChannels.push_back ({ { channel.id, channel.name, true }, std::move (chain) });
        }

        out.outputChannels = outputChannelCount (readOutputs (snapshot));

        /*  WHETHER ANYTHING IN THE SHOW WANTS SOUND: an interface that is not
            running is a problem for a show with sounds or mics, and not for a
            show that has none and never switched one on. */
        constexpr std::string_view cuePrefix = "/godot/cue/";
        constexpr std::string_view kindRow = "/kind";

        for (const auto* node : snapshot.all())
        {
            const auto& address = node->address;

            if (address.size() > cuePrefix.size() + kindRow.size()
                  && address.compare (0, cuePrefix.size(), cuePrefix) == 0
                  && address.compare (address.size() - kindRow.size(), kindRow.size(), kindRow) == 0)
            {
                const auto kind = text (node);

                if (kind == "media" || kind == "mic")
                {
                    out.needsSound = true;
                    break;
                }
            }
        }

        return out;
    }

    //==============================================================================
    std::vector<Problem> machineProblems (const tree::TreeSnapshot& snapshot, const ObjectIndex& index)
    {
        std::vector<Problem> out;

        /*  THE AUDIO INTERFACE: none while it is being applied - which is
            Apply's own "Restarting audio..." - and otherwise any reason it is
            not running, when something needs it to. */
        const auto status = text (snapshot, "/godot/audio/status");
        const auto running = status == "running";

        if (text (snapshot, "/godot/audio/settingsStatus") != "applying")
        {
            const auto error = text (snapshot, "/godot/audio/settingsError");

            if (! error.empty())
                add (out, pageName::audioInterface, "audio", "no audio", "The audio interface did not open: " + error);
            else if (status == "noClock")
                add (out, pageName::audioInterface, "audio", "no audio",
                     "The audio interface is disconnected: cues are paused, waiting for it and its clock");
            else if (! running && (index.needsSound || yes (snapshot, "/godot/audio/enabled")))
                add (out, pageName::audioInterface, "audio", "no audio",
                     "No audio interface is running: sound and mic cues play nothing");
        }

        /*  THE OUTPUTS AND THEIR PATCH past the interface - said only while one
            runs, which is when it has said how many channels it gives. */
        if (const auto hardware = integer (snapshot, "/godot/audio/hardwareOutputs"); running && hardware > 0)
        {
            if (index.outputChannels > hardware)
                add (out, pageName::outputs, "outputs", "outputs",
                     "The show's outputs take " + std::to_string (index.outputChannels) + " channels and the interface"
                       " gives " + std::to_string (hardware) + ": the last are not heard");

            for (const auto& entry : words (text (snapshot, "/godot/audio/outputPatch")))
                if (const auto channel = std::atoi (entry.c_str()); channel >= hardware)
                {
                    add (out, pageName::outputPatch, "outputPatch", "patch",
                         "An output is patched to channel " + std::to_string (channel + 1) + " and the interface gives "
                           + std::to_string (hardware) + ": it is not heard");
                    break;
                }
        }

        for (const auto& input : index.inputs)
            if (const auto why = text (snapshot, "/godot/input/" + input.id + "/problem"); ! why.empty())
                add (out, pageName::inputs, "input:" + input.id, "no input", "Input " + input.name + ": " + why);

        /*  A PLUGIN MISSING OR FAILED, on a rack channel or in the set. Loading
            and unloaded are not problems: the graph is being built, or there is
            no audio to build one on - which is said once, above. */
        const auto plugin = [&snapshot, &out] (const char* page, const ObjectIndex::Named& entry, const std::string& on)
        {
            const auto base = "/godot/plugin/" + entry.id + "/";
            const auto state = text (snapshot, base + "state");

            if (state != "missing" && state != "failed")
                return;

            const auto why = text (snapshot, base + "problem");
            add (out, page, "plugin:" + entry.id, state == "missing" ? "plugin missing" : "plugin failed",
                 "Plugin " + entry.name + on + (state == "missing" ? " is missing" : " failed")
                   + (why.empty() ? std::string {} : ": " + why));
        };

        for (const auto& [channel, chain] : index.rackChannels)
            for (const auto& entry : chain)
                plugin (pageName::rack, entry, " on rack channel " + channel.name);

        for (const auto& entry : index.setPlugins)
            plugin (pageName::plugins, entry, {});

        /*  THE PICTURES: the renderer failed; or, while it runs, an output not
            on its screen, an input with nothing arriving, an insert with
            nothing coming back. While it starts, nothing is said: every one of
            them says "the renderer not running" until it has. */
        const auto renderer = text (snapshot, "/godot/videoOutput/renderer");

        if (renderer == "failed")
        {
            const auto why = text (snapshot, "/godot/videoOutput/rendererProblem");
            add (out, pageName::video, "renderer", "no picture",
                 "The program that draws the pictures failed" + (why.empty() ? std::string {} : ": " + why));
        }
        else if (renderer == "running")
        {
            for (const auto& output : index.videoOutputs)
                if (const auto why = text (snapshot, "/godot/videoOutput/" + output.id + "/problem");
                      output.enabled && ! why.empty())
                    add (out, pageName::video, "videoOutput:" + output.id, "no screen", "Output " + output.name + ": " + why);

            for (const auto& input : index.videoInputs)
                if (const auto why = text (snapshot, "/godot/videoInput/" + input.id + "/problem");
                      input.enabled && ! why.empty() && ! yes (snapshot, "/godot/videoInput/" + input.id + "/connected"))
                    add (out, pageName::video, "videoInput:" + input.id, "no source", "Video input " + input.name + ": " + why);

            for (const auto& insert : index.videoInserts)
                if (const auto why = text (snapshot, "/godot/videoInsert/" + insert.id + "/problem");
                      ! why.empty() && ! yes (snapshot, "/godot/videoInsert/" + insert.id + "/connected"))
                    add (out, pageName::video, "videoInsert:" + insert.id, "no return", "Video insert " + insert.name + ": " + why);
        }

        /*  A NETWORK DEVICE that says it cannot be used as declared, or whose
            connection is down and being tried again. Not answering a listen
            is waiting, and is not said. */
        for (const auto& device : index.devices)
        {
            const auto base = "/godot/mount/" + device.id + "/";

            if (const auto why = text (snapshot, base + "problem"); ! why.empty())
                add (out, pageName::network, "device:" + device.id, "no device", "Device " + device.name + ": " + why);
            else if (const auto down = text (snapshot, base + "linkProblem");
                       ! down.empty() && text (snapshot, base + "link") == "retrying")
                add (out, pageName::network, "device:" + device.id, "no device", "Device " + device.name + ": " + down);
        }

        for (const auto& port : index.ports)
            if (const auto why = text (snapshot, "/godot/port/" + port.id + "/problem");
                  ! why.empty() && ! yes (snapshot, "/godot/port/" + port.id + "/bound"))
                add (out, pageName::midi, "port:" + port.id, "no port", "MIDI port " + port.name + ": " + why);

        for (const auto& surface : index.surfaces)
            if (const auto why = text (snapshot, "/godot/surface/" + surface.id + "/problem");
                  surface.enabled && ! why.empty() && ! yes (snapshot, "/godot/surface/" + surface.id + "/connected"))
                add (out, pageName::surfaces, "surface:" + surface.id, "no surface", "Surface " + surface.name + ": " + why);

        for (const auto& port : index.serials)
            if (const auto why = text (snapshot, "/godot/serial/" + port.id + "/problem");
                  ! why.empty() && text (snapshot, "/godot/serial/" + port.id + "/state") == "retrying")
                add (out, pageName::serial, "serial:" + port.id, "no port", "Serial port " + port.name + ": " + why);

        //  THE PAIR's own sentence (namespace draft §58): its banner says it too.
        if (const auto alarm = text (snapshot, "/godot/pair/alarm"); ! alarm.empty())
            add (out, pageName::redundancy, "pair", {}, alarm);

        return out;
    }

    //==============================================================================
    CueNeeds needsOf (const tree::TreeSnapshot& snapshot, const std::vector<Row>& rows, const ObjectIndex& index)
    {
        CueNeeds out;
        const auto devices = readDevices (snapshot);
        std::set<std::string> shown;

        for (const auto& row : rows)
            if (row.rowKind == RowKind::cue && ! row.derived && ! row.id.empty())
                shown.insert (row.id);

        const auto keep = [&out] (const std::string& id, std::vector<std::string> keys)
        {
            if (! keys.empty())
                out.keysOf[id] = std::move (keys);
        };

        for (const auto& row : rows)
        {
            if (row.rowKind != RowKind::cue || row.derived || row.id.empty() || ! row.runsNow())
                continue;

            keep (row.id, needsOfCue (snapshot, row.id, row.kind, row.mode, index, devices, out));

            if (! row.isGroup)
                continue;

            /*  WHAT A SHUT GROUP HIDES: its members that are not rows, and all
                of theirs - looked through by the order rows, so a fold costs
                no rebuild of the list to read. A member that is a row of its
                own is left to its own row. */
            std::vector<std::string> hidden;
            std::vector<std::string> waiting = membersOf (snapshot, row.id);
            std::set<std::string> seen;

            while (! waiting.empty())
            {
                const auto id = waiting.back();
                waiting.pop_back();

                if (shown.count (id) != 0 || ! seen.insert (id).second)
                    continue;

                const auto base = "/godot/cue/" + id + "/";
                const auto enabled = flag (snapshot, base + "enabled");

                if (enabled == Flag::no)
                    continue;

                const auto kind = text (snapshot, base + "kind");
                hidden.push_back (id);
                keep (id, needsOfCue (snapshot, id, kind, text (snapshot, base + "mode"), index, devices, out));

                if (kind == "group")
                    for (auto& member : membersOf (snapshot, id))
                        waiting.push_back (std::move (member));
            }

            if (! hidden.empty())
                out.hiddenIn[row.id] = std::move (hidden);
        }

        return out;
    }

    std::vector<Problem> fileProblems (const tree::TreeSnapshot& snapshot, const CueNeeds& needs)
    {
        std::vector<Problem> out;

        for (const auto& [cue, file] : needs.files)
            if (yes (snapshot, "/godot/cue/" + cue + "/missing"))
                out.push_back ({ {}, "file:" + cue, "missing",
                                 "Missing: " + file + " is not in the show's media, or Go.dot cannot read it - GO will"
                                 " play nothing of it" });

        return out;
    }

    //==============================================================================
    std::map<std::string, Mark> cueProblems (const CueNeeds& needs, const std::vector<Problem>& problems)
    {
        std::map<std::string, const Problem*> byKey;

        for (const auto& problem : problems)
            byKey.emplace (problem.key, &problem);

        const auto found = [&byKey] (const std::vector<std::string>& keys)
        {
            std::vector<const Problem*> out;

            for (const auto& key : keys)
                if (const auto at = byKey.find (key); at != byKey.end())
                    out.push_back (at->second);

            return out;
        };

        const auto sentences = [] (const std::vector<const Problem*>& list)
        {
            std::string out;

            for (const auto* problem : list)
                out += (out.empty() ? "" : "\n") + problem->sentence;

            return out;
        };

        std::map<std::string, Mark> out;

        for (const auto& [cue, keys] : needs.keysOf)
            if (const auto list = found (keys); ! list.empty())
                out[cue] = { Icon::warning, list.front()->word, sentences (list) };

        /*  A SHUT GROUP SAYS WHAT ITS HIDDEN MEMBERS NEED, after anything of its
            own: "inside", and how many. */
        for (const auto& [group, hidden] : needs.hiddenIn)
        {
            std::vector<const Problem*> inside;
            auto cues = 0;

            for (const auto& member : hidden)
                if (const auto keys = needs.keysOf.find (member); keys != needs.keysOf.end())
                    if (const auto list = found (keys->second); ! list.empty())
                    {
                        ++cues;

                        for (const auto* problem : list)
                            if (std::find (inside.begin(), inside.end(), problem) == inside.end())
                                inside.push_back (problem);
                    }

            if (cues == 0)
                continue;

            const auto said = (cues == 1 ? std::string ("A cue inside needs what is missing:\n")
                                         : std::to_string (cues) + " cues inside need what is missing:\n")
                                + sentences (inside);

            if (auto mark = out.find (group); mark != out.end())
                mark->second.meaning += "\n" + said;
            else
                out[group] = { Icon::warning, "inside", said };
        }

        return out;
    }

    std::map<std::string, Mark> withProblems (std::map<std::string, Mark> readiness,
                                              const std::map<std::string, Mark>& problems)
    {
        for (const auto& [cue, mark] : problems)
            readiness[cue] = mark;

        return readiness;
    }

    //==============================================================================
    std::vector<PageProblems> byPage (const std::vector<Problem>& problems)
    {
        //  In the settings window's own order, the files last.
        static const char* const order[] {
            pageName::playback, pageName::templates, pageName::redundancy,
            pageName::audioInterface, pageName::outputs, pageName::outputPatch, pageName::inputs, pageName::inputPatch,
            pageName::rack, pageName::plugins,
            pageName::video,
            pageName::network, pageName::midi, pageName::surfaces, pageName::timecode, pageName::serial,
            ""
        };

        std::vector<PageProblems> out;

        for (const auto* page : order)
        {
            PageProblems entry;
            entry.page = page;
            entry.icon = iconForPage (page);

            for (const auto& problem : problems)
            {
                if (problem.page != page)
                    continue;

                if (std::find (entry.sentences.begin(), entry.sentences.end(), problem.sentence) == entry.sentences.end())
                    entry.sentences.push_back (problem.sentence);

                if (problem.key.rfind ("file:", 0) == 0)
                    entry.cues.push_back (problem.key.substr (5));
            }

            if (! entry.sentences.empty())
                out.push_back (std::move (entry));
        }

        return out;
    }

    Icon iconForPage (std::string_view page)
    {
        const std::pair<std::string_view, Icon> icons[] {
            { pageName::audioInterface, Icon::sound }, { pageName::outputs, Icon::sends },
            { pageName::outputPatch, Icon::sends }, { pageName::inputs, Icon::mic },
            { pageName::inputPatch, Icon::mic }, { pageName::rack, Icon::rack },
            { pageName::plugins, Icon::plugin }, { pageName::video, Icon::video },
            { pageName::network, Icon::osc }, { pageName::midi, Icon::midi },
            { pageName::surfaces, Icon::surface }, { pageName::timecode, Icon::timecode },
            { pageName::serial, Icon::serial }, { pageName::redundancy, Icon::warning },
            { "", Icon::missing }
        };

        for (const auto& [name, icon] : icons)
            if (page == name)
                return icon;

        return Icon::none;
    }
}
