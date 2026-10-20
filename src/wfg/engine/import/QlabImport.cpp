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

#include <wfg/engine/import/QlabImport.h>

#include <wfg/engine/document/Sequence.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>
#include <wfg/engine/video/InstalledFonts.h>

#include <juce_graphics/juce_graphics.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <tuple>

namespace wfg::import::qlab
{
    namespace
    {
        std::string points (const std::vector<std::pair<double, double>>& pairs)
        {
            std::string out;

            for (const auto& [t, v] : pairs)
                out += (out.empty() ? "" : " ") + number (t, 4) + " " + number (v, 6);

            return out;
        }

        /*  EVERY AUDIO CUE OF THE LISTS TICKED, in show order - and every video
            cue's, whose files are found and copied as a sound's (§62, ALH). */
        void sounds (const Cue& cue, std::vector<const Cue*>& out)
        {
            if (cue.type == "Audio" || cue.type == "Video")
                out.push_back (&cue);

            for (const auto& child : cue.children)
                sounds (child, out);
        }

        /*  A PICTURE'S OR A MOVIE'S OWN SIZE, AND A MOVIE'S CODEC (§62, ALG,
            ALH): a still's from its own reader; a movie's from its QuickTime or
            MP4 boxes - the video track's header and sample description - walked
            without reading the frames, so a movie of gigabytes costs a few reads. */
        struct PictureFacts
        {
            int width = 0;
            int height = 0;
            std::string codec;          ///< a movie's four letters, "avc1", "apcn", "Hap1"; empty for a still
        };

        std::uint32_t big32 (const std::uint8_t* p)
        {
            return (static_cast<std::uint32_t> (p[0]) << 24) | (static_cast<std::uint32_t> (p[1]) << 16)
                 | (static_cast<std::uint32_t> (p[2]) << 8) | static_cast<std::uint32_t> (p[3]);
        }

        /*  The boxes inside `data` from `begin` to `end`, each its type and where
            its contents lie; a box that claims more than there is ends the walk. */
        std::vector<std::tuple<std::string, std::size_t, std::size_t>> boxesIn (const std::vector<std::uint8_t>& data,
                                                                                std::size_t begin, std::size_t end)
        {
            std::vector<std::tuple<std::string, std::size_t, std::size_t>> boxes;
            auto at = begin;

            while (at + 8 <= end && boxes.size() < 4096)
            {
                std::uint64_t size = big32 (data.data() + at);
                const std::string type (reinterpret_cast<const char*> (data.data() + at + 4), 4);
                std::size_t header = 8;

                if (size == 1 && at + 16 <= end)
                {
                    size = (static_cast<std::uint64_t> (big32 (data.data() + at + 8)) << 32) | big32 (data.data() + at + 12);
                    header = 16;
                }
                else if (size == 0)
                    size = end - at;

                if (size < header || size > end - at)
                    break;

                boxes.emplace_back (type, at + header, at + static_cast<std::size_t> (size));
                at += static_cast<std::size_t> (size);
            }
            return boxes;
        }

        PictureFacts movieFacts (const juce::File& file)
        {
            PictureFacts facts;
            juce::FileInputStream in (file);
            if (! in.openedOk())
                return facts;

            //  The top level, read box by box until `moov`: a movie's frames are skipped, never read.
            std::vector<std::uint8_t> moov;
            while (! in.isExhausted() && moov.empty())
            {
                std::uint8_t head[16] {};
                const auto start = in.getPosition();
                if (in.read (head, 8) != 8)
                    break;

                std::uint64_t size = big32 (head);
                std::int64_t header = 8;
                if (size == 1)
                {
                    if (in.read (head + 8, 8) != 8)
                        break;
                    size = (static_cast<std::uint64_t> (big32 (head + 8)) << 32) | big32 (head + 12);
                    header = 16;
                }
                if (size == 0)
                    size = static_cast<std::uint64_t> (in.getTotalLength() - start);
                if (size < static_cast<std::uint64_t> (header))
                    break;

                if (std::string (reinterpret_cast<const char*> (head + 4), 4) == "moov")
                {
                    const auto contents = size - static_cast<std::uint64_t> (header);
                    if (contents > 64u * 1024u * 1024u)
                        break;
                    moov.resize (static_cast<std::size_t> (contents));
                    if (in.read (moov.data(), static_cast<int> (moov.size())) != static_cast<int> (moov.size()))
                        moov.clear();
                    break;
                }
                in.setPosition (start + static_cast<std::int64_t> (size));
            }
            if (moov.empty())
                return facts;

            const auto find = [&moov] (std::size_t begin, std::size_t end, const char* type) -> std::pair<std::size_t, std::size_t>
            {
                for (const auto& [t, from, to] : boxesIn (moov, begin, end))
                    if (t == type)
                        return { from, to };
                return { 0, 0 };
            };

            for (const auto& [type, from, to] : boxesIn (moov, 0, moov.size()))
            {
                if (type != "trak")
                    continue;

                const auto [mdiaFrom, mdiaTo] = find (from, to, "mdia");
                const auto [hdlrFrom, hdlrTo] = find (mdiaFrom, mdiaTo, "hdlr");
                if (hdlrTo < hdlrFrom + 12 || std::string (reinterpret_cast<const char*> (moov.data() + hdlrFrom + 8), 4) != "vide")
                    continue;

                //  The track header: its size, 16.16, after a header longer in version 1.
                const auto [tkhdFrom, tkhdTo] = find (from, to, "tkhd");
                const auto at = tkhdFrom + (moov[tkhdFrom] == 1 ? 88u : 76u);
                if (tkhdTo >= at + 8)
                {
                    facts.width = static_cast<int> (big32 (moov.data() + at) >> 16);
                    facts.height = static_cast<int> (big32 (moov.data() + at + 4) >> 16);
                }

                //  The sample description's first entry: its four letters.
                const auto [minfFrom, minfTo] = find (mdiaFrom, mdiaTo, "minf");
                const auto [stblFrom, stblTo] = find (minfFrom, minfTo, "stbl");
                const auto [stsdFrom, stsdTo] = find (stblFrom, stblTo, "stsd");
                if (stsdTo >= stsdFrom + 16)
                    facts.codec = std::string (reinterpret_cast<const char*> (moov.data() + stsdFrom + 12), 4);
                break;
            }
            return facts;
        }

        PictureFacts pictureFacts (const juce::File& file, bool still)
        {
            if (! still)
                return movieFacts (file);

            PictureFacts facts;
            const auto image = juce::ImageFileFormat::loadFrom (file);
            facts.width = image.getWidth();
            facts.height = image.getHeight();
            return facts;
        }

        bool isStillFile (const juce::File& file)
        {
            return file.hasFileExtension ("png;jpg;jpeg;gif;tif;tiff;bmp;psd;heic;heif;webp;tga;exr");
        }

        //======================================================================
        struct Builder
        {
            const Workspace& workspace;
            const Plan& plan;
            const std::map<std::string, std::pair<std::string, int>>& media;
            doc::ShowDocument& document;

            BuiltShow built;
            Identities ids { "wfg-qlab:" };
            std::map<std::string, std::string> made;        ///< plan key to the identifier written
            std::vector<std::pair<std::string, std::string>> targets;   ///< cue identifier, target key

            void said (Note::Kind kind, const std::string& where, const std::string& text)
            {
                built.notes.push_back ({ kind, -1, where, text });
            }

            bool write (const std::string& address, const std::string& text, const std::string& where)
            {
                const auto written = document.setAttribute (address, text);

                if (! written.ok)
                    said (Note::Kind::dropped, where, address + " could not be written (" + written.reason + "): "
                                                      + text.substr (0, 80));

                return written.ok;
            }

            void item (const Item& item, const std::string& parent)
            {
                const auto id = ids.of (item.key);
                auto attributes = item.attributes;

                if (item.kind == "media")
                {
                    const auto named = media.find (item.cueId);
                    attributes.push_back ({ "file", named != media.end() ? named->second.first : item.file.name() });

                    if (named != media.end() && named->second.second > 0)
                        attributes.push_back ({ "channels", std::to_string (named->second.second) });
                }

                //  A PICTURE, A MOVIE OR A TEXT (§62): its canvas, and a picture's or a movie's file.
                if (item.kind == "video")
                {
                    if (const auto canvas = made.find (item.canvas); canvas != made.end())
                        attributes.push_back ({ "canvas", canvas->second });

                    if (! item.cueId.empty())
                    {
                        const auto named = media.find (item.cueId);
                        attributes.push_back ({ "file", named != media.end() ? named->second.first : item.file.name() });
                    }
                }

                const auto created = document.createCue (parent, doc::endOfSequence, item.kind, item.name, id, attributes);

                if (! created.ok)
                {
                    said (Note::Kind::dropped, item.name, "the cue could not be made (" + created.reason + ")");
                    return;
                }

                made[item.key] = id;
                ++built.cues;

                if (! item.target.empty())
                    targets.push_back ({ id, item.target });

                //  --- A sound's ranges and routes ------------------------------------
                for (std::size_t at = 0; at < item.ranges.size(); ++at)
                {
                    const auto& range = item.ranges[at];
                    const auto made2 = document.createRange (id, range.in, range.out,
                                                             ids.of (item.key + ":range:" + std::to_string (at)),
                                                             range.toEnd);

                    if (! made2.ok)
                    {
                        said (Note::Kind::dropped, item.name, "a range could not be made (" + made2.reason + ")");
                        continue;
                    }

                    if (range.loops != 1)
                        write ("/godot/range/" + made2.id + "/loops", std::to_string (range.loops), item.name);
                }

                if (item.kind == "media")
                {
                    const auto named = media.find (item.cueId);
                    const auto channels = named != media.end() ? named->second.second : 0;

                    if (item.defaultRoute && channels > 0)
                    {
                        if (! document.defaultMediaRoute (id, channels, ids.of (item.key + ":route")).ok)
                            said (Note::Kind::dropped, item.name, "Go.dot's default route could not be given: the show "
                                                                  "has no output to give it");
                    }

                    for (const auto& route : item.routes)
                    {
                        const auto bus = made.find (route.bus);

                        if (bus == made.end() || static_cast<int> (route.gains.size()) != channels)
                            continue;

                        const auto routed = document.createRoute (id, bus->second, ids.of (item.key + ":" + route.bus));

                        if (! routed.ok)
                        {
                            said (Note::Kind::dropped, item.name, "a route could not be made (" + routed.reason + ")");
                            continue;
                        }

                        std::string gains;

                        for (const auto g : route.gains)
                            gains += (gains.empty() ? "" : " ") + number (g, 6);

                        write ("/godot/route/" + routed.id + "/gains", gains, item.name);
                    }
                }

                //  --- An OSC cue's curves ---------------------------------------------
                for (const auto& curve : item.curves)
                {
                    const auto made2 = document.createCurve (id, curve.arg, ids.of (item.key + ":curve:" + std::to_string (curve.arg)));

                    if (! made2.ok)
                    {
                        said (Note::Kind::dropped, item.name, "its curve could not be made (" + made2.reason + ")");
                        continue;
                    }

                    write ("/godot/curve/" + made2.id + "/points", points (curve.points), item.name);
                }

                for (const auto& child : item.children)
                    this->item (child, id);
            }

            void run()
            {
                //  --- The voices: one per sound, with room ---------------------------
                {
                    int widest = 2;

                    for (const auto& [cue, file] : media)
                        widest = std::max (widest, file.second);

                    const auto voices = std::clamp (static_cast<int> (media.size()) + 8, 32, 256);
                    write ("/godot/audio/tracks", std::to_string (voices), {});

                    if (widest > 2)
                        write ("/godot/audio/channelsPerTrack", std::to_string (std::min (widest, 64)), {});
                }

                //  --- The outputs: a mono direct bus per QLab cue output (ZU) ----------
                std::string patch;

                for (const auto& bus : plan.buses)
                {
                    const auto id = ids.of (bus.key);
                    const auto created = document.createBus ("direct", 1, -1, id);

                    if (! created.ok)
                    {
                        said (Note::Kind::dropped, bus.name, "its output could not be made (" + created.reason + ")");
                        continue;
                    }

                    made[bus.key] = id;
                    write ("/godot/bus/" + id + "/name", bus.name, bus.name);
                    patch += (patch.empty() ? "" : " ") + std::to_string (bus.channel);
                }

                if (! patch.empty())
                    write ("/godot/audio/outputPatch", patch, {});

                //  --- The devices: an opaque mount per network patch (ZW) --------------
                for (const auto& device : plan.devices)
                {
                    if (device.prefix.empty())
                        continue;

                    const auto id = ids.of (device.key);
                    const auto created = document.createMount (device.prefix, {}, id);

                    if (! created.ok)
                    {
                        said (Note::Kind::dropped, device.name, "its device could not be made (" + created.reason + ")");
                        continue;
                    }

                    made[device.key] = id;

                    if (! device.name.empty())
                        write ("/godot/mount/" + id + "/name", device.name, device.name);

                    write ("/godot/mount/" + id + "/host", device.host, device.name);

                    if (device.port > 0)
                        write ("/godot/mount/" + id + "/port", std::to_string (device.port), device.name);

                    if (device.tcp)
                        said (Note::Kind::approximated, device.name, "QLab sent to it over TCP; Go.dot sends OSC over UDP");
                }

                //  --- The canvases, and their outputs with no display chosen (§62, ALF) ---
                for (const auto& canvas : plan.canvases)
                {
                    const auto id = ids.of (canvas.key);
                    const auto created = document.createCanvas (canvas.name, id);

                    if (! created.ok)
                    {
                        said (Note::Kind::dropped, canvas.name, "its canvas could not be made (" + created.reason + ")");
                        continue;
                    }

                    made[canvas.key] = id;
                    write ("/godot/canvas/" + id + "/width", std::to_string (canvas.width), canvas.name);
                    write ("/godot/canvas/" + id + "/height", std::to_string (canvas.height), canvas.name);
                }

                for (const auto& output : plan.outputs)
                {
                    const auto canvas = made.find (output.canvas);
                    if (! document.createVideoOutput (output.name, canvas != made.end() ? canvas->second : std::string {},
                                                      ids.of (output.key)).ok)
                        said (Note::Kind::dropped, output.name, "its video output could not be made");
                }

                //  --- The lists and their cues ------------------------------------------
                for (const auto& list : plan.lists)
                {
                    const auto id = ids.of (list.key);

                    if (! document.createList (list.name, id).ok)
                    {
                        said (Note::Kind::dropped, list.name, "the cue list could not be made");
                        continue;
                    }

                    ++built.lists;

                    for (const auto& each : list.items)
                        item (each, id);

                    /*  STANDBY ON THE FIRST CUE, where a show made by hand has it:
                        with no standby, GO has nothing to fire. */
                    if (! list.items.empty())
                        if (const auto first = made.find (list.items.front().key); first != made.end())
                            write ("/godot/list/" + id + "/standby", first->second, list.name);
                }

                /*  TARGETS ONCE EVERY CUE EXISTS: QLab may aim a fade at a sound
                    further down the list. */
                for (const auto& [id, key] : targets)
                {
                    const auto target = made.find (key);

                    if (target == made.end())
                    {
                        said (Note::Kind::dropped, id, "its target was not imported, so it is aimed at nothing");
                        continue;
                    }

                    write ("/godot/cue/" + id + "/target", target->second, id);
                }
            }
        };

        //======================================================================
        std::string reportText (const juce::File& file, const Workspace& workspace, const Plan& plan,
                                const std::vector<Note>& notes, const ImportOutcome& outcome)
        {
            std::ostringstream out;

            out << "# Import report: " << file.getFileNameWithoutExtension().toStdString() << "\n\n";
            out << "Imported by Go.dot from `" << file.getFileName().toStdString() << "` (QLab " << workspace.version
                << "), " << juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H:%M").toStdString() << ".\n\n";
            out << "The import never hides an approximation: everything below came over changed, did not come over, "
                   "or is a fact to know before the show is run (namespace draft §46, ZZ). The file was read, never "
                   "run: a script stays text.\n\n";

            out << "## What came over\n\n";
            out << "- " << outcome.lists << " cue list(s), " << outcome.cues << " QLab cue(s).\n";
            out << "- " << outcome.placeholders << " memo(s) standing in for a kind Go.dot has no equivalent of - each "
                   "named `[QLab] ...`, the original in its notes.\n";

            if (! plan.buses.empty())
            {
                out << "\n## The outputs\n\nA mono bus for each QLab cue output in use, on the interface output of the "
                       "same number.\n\n| Bus | Interface output |\n|---|---|\n";

                for (const auto& bus : plan.buses)
                    out << "| " << bus.name << " | " << (bus.channel < 0 ? std::string ("none") : std::to_string (bus.channel + 1)) << " |\n";
            }

            if (! plan.devices.empty())
            {
                out << "\n## The devices\n\nAn opaque device for each QLab network patch a cue sends through: Go.dot "
                       "sends what each cue says and asks nothing back. Describe one with its OSCQuery namespace to "
                       "have its addresses checked.\n\n| Device | Where | Prefix |\n|---|---|---|\n";

                for (const auto& device : plan.devices)
                    out << "| " << device.name << " | " << device.host << ":" << device.port << " | `" << device.prefix << "` |\n";
            }

            if (! plan.canvases.empty())
            {
                out << "\n## The canvases\n\nA canvas for each QLab stage, of its size, and a video output for each "
                       "output QLab drew to, once each - none showing anything until a projector is picked for it in "
                       "Show settings, Video.\n\n| Canvas | Size | Drawn to |\n|---|---|---|\n";

                for (const auto& canvas : plan.canvases)
                {
                    std::string outputs;
                    for (const auto& output : canvas.outputs)
                        outputs += (outputs.empty() ? "" : ", ") + output;
                    out << "| " << canvas.name << " | " << canvas.width << " x " << canvas.height << " | "
                        << (outputs.empty() ? std::string ("none") : outputs) << " |\n";
                }
            }

            if (! plan.fonts.empty())
            {
                out << "\n## The fonts\n\nEach font a text cue names, as QLab names it, and the family it draws in "
                       "here. A missing one draws in the system's sans-serif until it is installed, or carried in the "
                       "show from a machine that has it (the font menu's Carry it in the show).\n\n"
                       "| QLab's font | Family | Here |\n|---|---|---|\n";

                for (const auto& font : plan.fonts)
                    out << "| " << font.postScript << " | " << font.family << " | "
                        << (font.where == "installed" ? "installed on this machine" : "missing") << " |\n";
            }

            const auto listName = [&workspace] (int index)
            {
                return index >= 0 && static_cast<std::size_t> (index) < workspace.lists.size()
                         ? "\"" + workspace.lists[static_cast<std::size_t> (index)].name + "\"" : std::string {};
            };

            for (const auto kind : { Note::Kind::approximated, Note::Kind::dropped, Note::Kind::info })
            {
                std::vector<const Note*> these;

                for (const auto& note : notes)
                    if (note.kind == kind)
                        these.push_back (&note);

                if (these.empty())
                    continue;

                out << "\n## " << (kind == Note::Kind::approximated ? "Approximated"
                                   : kind == Note::Kind::dropped     ? "Not imported"
                                                                     : "To know") << "\n\n";

                for (const auto* note : these)
                {
                    const auto list = listName (note->scene);
                    out << "- " << (list.empty() ? std::string {} : list + ", ")
                        << (note->track.empty() ? std::string ("the workspace") : "\"" + note->track + "\"") << ": "
                        << note->text << "\n";
                }
            }

            if (! outcome.missingMedia.empty())
            {
                out << "\n## Files not found\n\nEach keeps its cue, naming the file; put the file in the show's "
                       "`media/` and it plays.\n\n";

                for (const auto& missing : outcome.missingMedia)
                    out << "- `" << missing << "`\n";
            }

            return out.str();
        }
    }

    //==========================================================================
    std::string idFor (const std::string& key)
    {
        return import::idFor ("wfg-qlab:", key);
    }

    std::optional<std::set<int>> parseLists (const std::string& text)
    {
        std::set<int> out;
        juce::StringArray parts;
        parts.addTokens (juce::String (text), ",", {});
        parts.trim();
        parts.removeEmptyStrings();

        if (parts.isEmpty())
            return std::nullopt;

        for (const auto& part : parts)
        {
            const auto from = part.upToFirstOccurrenceOf ("-", false, false).trim();
            const auto to = part.contains ("-") ? part.fromFirstOccurrenceOf ("-", false, false).trim() : from;

            if (from.isEmpty() || to.isEmpty() || ! from.containsOnly ("0123456789") || ! to.containsOnly ("0123456789"))
                return std::nullopt;

            const auto low = from.getIntValue();
            const auto high = to.getIntValue();

            if (low < 1 || high < low)
                return std::nullopt;

            for (auto list = low; list <= high; ++list)
                out.insert (list - 1);
        }

        return out;
    }

    juce::File findMedia (const FileRef& file, const juce::File& workspaceFolder)
    {
        std::vector<juce::File> folders;

        if (file.relativeToParent)
            folders.push_back (workspaceFolder.getParentDirectory());

        folders.push_back (workspaceFolder);

        auto found = findFile ({ file.relativePath, file.absolutePath, file.name(), 0 }, folders);

        /*  A QLAB 4 PATH FROM THE FOLDER ABOVE whose first folder was renamed
            since: the rest of it, from the workspace's own folder. */
        if (found == juce::File() && file.relativeToParent)
            if (const auto slash = file.relativePath.find ('/'); slash != std::string::npos)
                found = findFile ({ file.relativePath.substr (slash + 1), {}, file.name(), 0 }, { workspaceFolder });

        return found;
    }

    BuiltShow build (const Workspace& workspace, const Plan& plan,
                     const std::map<std::string, std::pair<std::string, int>>& media, doc::ShowDocument& document)
    {
        Builder builder { workspace, plan, media, document, {}, Identities ("wfg-qlab:"), {}, {} };
        builder.run();
        return std::move (builder.built);
    }

    ImportOutcome importWorkspace (const juce::File& file, const ImportOptions& options)
    {
        ImportOutcome outcome;

        const auto say = [&options] (const std::string& sentence)
        {
            if (options.progress)
                options.progress (sentence);
        };

        say ("reading " + file.getFileName().toStdString());
        const auto read = readWorkspace (file);

        if (! read.workspace.has_value())
        {
            outcome.error = read.error;
            return outcome;
        }

        const auto& workspace = *read.workspace;

        /*  NEVER OVER SOMEBODY'S SHOW. */
        if (options.into == juce::File())
        {
            outcome.error = "no folder to import into";
            return outcome;
        }

        if (holdsAShow (options.into))
        {
            outcome.error = "the folder " + options.into.getFullPathName().toStdString()
                            + " already holds a show; import into a new one";
            return outcome;
        }

        for (const auto index : options.lists)
            if (index < 0 || static_cast<std::size_t> (index) >= workspace.lists.size())
            {
                outcome.error = "the workspace has no cue list " + std::to_string (index + 1);
                return outcome;
            }

        //  --- Each sound found and placed (ZT) -----------------------------------
        std::vector<const Cue*> audio;

        for (std::size_t at = 0; at < workspace.lists.size(); ++at)
            if (options.lists.empty() || options.lists.count (static_cast<int> (at)) != 0)
                sounds (workspace.lists[at], audio);

        const auto mediaFolder = options.into.getChildFile ("media");

        if (! audio.empty() && options.copyMedia && ! mediaFolder.createDirectory())
        {
            outcome.error = "the folder " + mediaFolder.getFullPathName().toStdString() + " could not be made";
            return outcome;
        }

        std::map<std::string, std::pair<std::string, int>> media;
        std::vector<Note> notes;
        MediaBook book;
        WalkOptions walkOptions;
        walkOptions.lists = options.lists;

        for (const auto* sound : audio)
        {
            const auto source = findMedia (sound->file, file.getParentDirectory());

            if (source == juce::File())
            {
                const auto named = sound->file.relativePath.empty() ? sound->file.name() : sound->file.relativePath;

                if (std::find (outcome.missingMedia.begin(), outcome.missingMedia.end(), named) == outcome.missingMedia.end())
                    outcome.missingMedia.push_back (named);

                continue;
            }

            const auto placed = placeFile (source, mediaFolder, options.copyMedia, book, say);

            if (placed.copyFailed)
            {
                notes.push_back ({ Note::Kind::dropped, -1, placed.name, "the file could not be copied into media/" });
                outcome.missingMedia.push_back (placed.name);
            }

            if (sound->type == "Video")
            {
                //  A PICTURE'S OWN SIZE, which a cue QLab drew at its own size is scaled from (ALG); a movie not in HAP said.
                const auto facts = pictureFacts (source, isStillFile (source));
                media[sound->id] = { placed.name, 0 };

                if (facts.width > 0 && facts.height > 0)
                    walkOptions.pictureSizes[sound->id] = { facts.width, facts.height };

                if (! facts.codec.empty() && facts.codec.rfind ("Hap", 0) != 0)
                    notes.push_back ({ Note::Kind::info, -1, placed.name, "a movie in " + facts.codec + ", which Go.dot shows "
                                       "as a preview until it is converted to HAP, which also makes its locked sound" });
                continue;
            }

            media[sound->id] = { placed.name, placed.channels };

            if (placed.channels > 0)
                walkOptions.channels[sound->id] = placed.channels;
        }

        /*  A TEXT CUE'S FONT, as this machine has it under QLab's PostScript
            name (§62, ALI): asked of the system here, never by the walk. */
        walkOptions.fontOf = [] (const std::string& postScript) -> std::optional<FontFace>
        {
            if (const auto face = video::installed::byPostScript (postScript))
                return FontFace { face->family, face->bold, face->italic };
            return std::nullopt;
        };

        //  --- Walked, built, saved, reported ------------------------------------
        say ("walking the cues");
        const auto plan = walk (workspace, walkOptions);
        notes.insert (notes.end(), plan.notes.begin(), plan.notes.end());

        for (const auto& problem : workspace.problems)
            notes.push_back ({ Note::Kind::info, -1, {}, "the workspace: " + problem });

        say ("writing the show");
        doc::ShowDocument document;
        const auto built = build (workspace, plan, media, document);
        notes.insert (notes.end(), built.notes.begin(), built.notes.end());

        const auto saved = saveShow (document, options.into, "workspace");
        outcome.problems = saved.problems;

        if (! saved.ok)
        {
            outcome.error = saved.error;
            return outcome;
        }

        outcome.ok = true;
        outcome.show = options.into;
        outcome.lists = built.lists;
        outcome.cues = plan.cues;
        outcome.placeholders = plan.placeholders;

        for (const auto& note : notes)
        {
            outcome.approximated += note.kind == Note::Kind::approximated ? 1 : 0;
            outcome.dropped += note.kind == Note::Kind::dropped ? 1 : 0;
        }

        outcome.report = options.into.getChildFile ("import-report.md");
        outcome.report.replaceWithText (juce::String::fromUTF8 (reportText (file, workspace, plan, notes, outcome).c_str()),
                                        false, false, "\n");
        say ("done");
        return outcome;
    }
}
