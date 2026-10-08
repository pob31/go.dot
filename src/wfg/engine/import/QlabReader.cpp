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

#include <wfg/engine/import/QlabReader.h>

#include <wfg/engine/import/Bplist.h>

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string_view>

namespace wfg::import::qlab
{
    namespace
    {
        using plist::Archive;
        using Handle = Archive::Handle;

        /*  ONE ARCHIVE, AND THE PLAIN QUESTIONS THE READER ASKS OF IT: a field
            as a number, a whole number, a flag or text, with the default the
            cue has when QLab did not write it. */
        struct Fields
        {
            const Archive& archive;

            Handle at (Handle object, std::string_view key) const { return archive.field (object, key); }

            double number (Handle object, std::string_view key, double otherwise = 0.0) const
            {
                return archive.number (at (object, key)).value_or (otherwise);
            }

            int whole (Handle object, std::string_view key, int otherwise = 0) const
            {
                const auto value = archive.integer (at (object, key));
                return value.has_value() ? static_cast<int> (std::clamp<std::int64_t> (*value, -2147483647, 2147483647))
                                         : otherwise;
            }

            bool flag (Handle object, std::string_view key, bool otherwise = false) const
            {
                return archive.boolean (at (object, key)).value_or (otherwise);
            }

            std::string text (Handle object, std::string_view key) const
            {
                return archive.text (at (object, key)).value_or (std::string {});
            }

            /*  A KEY OF A DICTIONARY, plain or archived, by name. */
            Handle entry (Handle dictionary, std::string_view key) const
            {
                for (const auto& [name, value] : archive.entries (dictionary))
                    if (name == key)
                        return value;

                return Archive::none;
            }

            /*  TEXT OR A NUMBER, as text: QLab 4 writes a patch as its number. */
            std::string idText (Handle object, std::string_view key) const
            {
                const auto value = at (object, key);

                if (const auto words = archive.text (value); words.has_value())
                    return *words;

                if (const auto number = archive.integer (value); number.has_value())
                    return std::to_string (*number);

                return {};
            }
        };

        Shape shapeOf (const Fields& f, Handle shape)
        {
            Shape out;
            out.type = f.whole (shape, "type", 1);
            out.parameter = f.number (shape, "curveParameter", 1.0);

            for (const auto entry : f.archive.items (f.at (shape, "shapeEntries")))
                out.points.push_back ({ f.number (entry, "t"), f.number (entry, "v") });

            return out;
        }

        /*  THE CUES OF ONE ARCHIVE, a group's members read in turn. Each object
            is read once: an archive whose groups hold one another - which QLab
            never writes, and a damaged file might - stops at the second visit
            rather than going round. */
        struct CueReader
        {
            const Fields& f;
            int major = 5;
            std::vector<std::string>& problems;
            std::set<Handle> seen;

            Cue read (Handle handle, int depth)
            {
                Cue cue;
                auto type = f.archive.className (handle);

                if (type.size() > 3 && type.compare (type.size() - 3, 3, "Cue") == 0)
                    type.resize (type.size() - 3);

                cue.type = type;
                cue.id = f.text (handle, "uniqueID");
                cue.name = f.text (handle, "name");
                cue.number = f.text (handle, "number");
                cue.notes = f.text (handle, "notes");
                cue.armed = f.flag (handle, "armed", true);
                cue.preWait = f.number (handle, "preWait");
                cue.postWait = f.number (handle, "postWait");
                cue.duration = f.number (handle, "duration");
                cue.continueMode = f.whole (handle, "continueMode");
                cue.target = f.text (handle, "cueTargetUniqueID");

                cue.hotkeyTrigger = f.flag (handle, "useHotKey");
                cue.midiTrigger = f.flag (handle, "useMIDITrigger");
                cue.wallClockTrigger = f.flag (handle, "useWallClock");
                cue.timecodeTrigger = f.flag (handle, "useTimecode");

                if (type == "Group")
                    readGroup (handle, cue, depth);
                else if (type == "Audio")
                    readAudio (handle, cue);
                else if (type == "Fade")
                    readFade (handle, cue);
                else if (type == "OSC")
                    readNetwork (handle, cue);
                else if (type == "Mic")
                {
                    cue.inputPatch = f.idText (handle, "audioInputPatchID");
                    cue.inputChannel = f.whole (handle, "channelOffset");
                    cue.inputChannels = f.whole (handle, "channels");
                }
                else if (type == "Script")
                {
                    cue.source = f.text (handle, "source");
                }

                return cue;
            }

            void readGroup (Handle handle, Cue& cue, int depth)
            {
                cue.groupMode = f.whole (handle, "groupMode", 0);
                cue.playlistLoop = f.flag (handle, "playlistLoop");
                cue.playlistShuffle = f.flag (handle, "playlistShuffle");
                cue.playlistCrossfade = f.flag (handle, "playlistCrossfade");

                if (depth > 64)
                {
                    problems.push_back ("a group nested more than 64 deep was read without its members");
                    return;
                }

                for (const auto member : f.archive.items (f.at (handle, "cues")))
                {
                    if (member == Archive::none || ! seen.insert (member).second)
                    {
                        problems.push_back ("a group's member that is already somewhere else in the workspace was left out");
                        continue;
                    }

                    cue.children.push_back (read (member, depth + 1));
                }
            }

            void readAudio (Handle handle, Cue& cue)
            {
                const auto alias = f.at (handle, "fileTarget");
                cue.file.absolutePath = f.text (alias, "lastKnownPath");
                cue.file.relativePath = f.text (alias, "relativePath");

                /*  QLAB 4 KEEPS THE RELATIVE PATH ON THE CUE, from the folder
                    above the workspace (§46.5). */
                if (cue.file.relativePath.empty())
                {
                    cue.file.relativePath = f.text (handle, "relativePath");
                    cue.file.relativeToParent = ! cue.file.relativePath.empty();
                }

                cue.audioPatch = f.idText (handle, major >= 5 ? "audioOutputPatchID" : "patch");
                cue.levels = levelsOf (f.at (handle, "levels"));
                cue.rate = f.number (handle, "rate", 1.0);
                cue.pitchFollowsRate = f.flag (handle, "doPitchShift", true);
                cue.startTime = f.number (handle, "startTime");
                cue.endTime = f.number (handle, "endTime");
                cue.fileDuration = f.number (handle, "lastSeenFileDuration");
                cue.playCount = f.whole (handle, "playCount", 1);
                cue.infiniteLoop = f.flag (handle, "infiniteLoop");

                for (const auto slice : f.archive.items (f.at (handle, "slices")))
                    cue.slices.push_back ({ f.number (slice, "time"), f.whole (slice, "playCount", 1),
                                            f.flag (slice, "infiniteLoop") });

                const auto last = f.at (handle, "lastSlice");
                cue.lastSlice = { f.number (last, "time"), f.whole (last, "playCount", 1), f.flag (last, "infiniteLoop") };
            }

            std::vector<Level> levelsOf (Handle matrix)
            {
                std::vector<Level> out;

                for (const auto& [key, knob] : f.archive.entries (f.at (matrix, "entries")))
                    out.push_back ({ f.whole (knob, "row"), f.whole (knob, "column"), f.number (knob, "initialLevel", 1.0) });

                /*  IN ORDER, row then column - the dictionary's own order is
                    whatever the archiver's hash table had. */
                std::sort (out.begin(), out.end(), [] (const Level& a, const Level& b)
                {
                    return a.row != b.row ? a.row < b.row : a.column < b.column;
                });

                return out;
            }

            void readFade (Handle handle, Cue& cue)
            {
                const auto fade = f.at (handle, "fade");
                cue.stopTargetWhenDone = f.flag (handle, "stopTargetWhenDone");
                cue.fadesRate = f.flag (handle, "doRate");
                cue.rate = f.number (handle, "rate", 1.0);

                /*  ABSOLUTE OR RELATIVE: QLab 4 says it in words, QLab 5 as a
                    number (§46.5). */
                if (const auto words = f.archive.text (f.at (fade, "fadeType")); words.has_value())
                    cue.absolute = *words != "relative";
                else
                    cue.absolute = f.whole (fade, "fadeMode", 1) != 0;

                for (const auto& [key, entry] : f.archive.entries (f.at (fade, "entries")))
                    cue.fadeLevels.push_back ({ f.whole (entry, "row"), f.whole (entry, "column"),
                                                f.number (entry, "startValue"), f.number (entry, "endValue") });

                std::sort (cue.fadeLevels.begin(), cue.fadeLevels.end(), [] (const FadeLevel& a, const FadeLevel& b)
                {
                    return a.row != b.row ? a.row < b.row : a.column < b.column;
                });

                cue.shape = shapeOf (f, upShape (fade));
            }

            /*  THE RISING SHAPE: under `shapes` in QLab 5, on the fade itself in 4. */
            Handle upShape (Handle fade) const
            {
                const auto shapes = f.at (fade, "shapes");
                return f.at (shapes != Archive::none ? shapes : fade, "upShape");
            }

            void readNetwork (Handle handle, Cue& cue)
            {
                if (major >= 5)
                {
                    cue.message = f.text (handle, "oscString");
                    cue.networkPatch = f.idText (handle, "networkPatchID");
                }
                else
                {
                    cue.message = f.text (handle, "rawString");
                    cue.networkPatch = f.idText (handle, "patch");
                    cue.messageType = f.whole (handle, "messageType", 2);
                }

                cue.networkFadeType = f.whole (handle, "fadeType");
                cue.fadeFrom = f.number (handle, "startValue");
                cue.fadeTo = f.number (handle, "endValue");
                cue.fadeFloats = f.flag (handle, "fadingFloats", true);
                cue.shape = shapeOf (f, upShape (f.at (handle, "fade")));
            }
        };

        std::optional<Archive> archiveOf (const std::uint8_t* bytes, std::size_t size, std::string& error)
        {
            auto parsed = plist::parse (bytes, size);

            if (! parsed.list.has_value())
            {
                error = parsed.error;
                return std::nullopt;
            }

            return Archive::from (std::move (*parsed.list), error);
        }

        //  --- The workspace's settings -----------------------------------------

        void readAudioSettings (const Fields& f, Handle audio, Workspace& workspace)
        {
            workspace.minVolume = f.archive.number (f.entry (audio, "minVolume")).value_or (workspace.minVolume);

            //  QLab 5: patches with identifiers, each naming its cue outputs.
            for (const auto patch : f.archive.items (f.entry (audio, "audioOutputPatches")))
            {
                AudioPatch out;
                out.id = f.text (patch, "uniqueID");
                out.name = f.text (patch, "name");
                out.outputs = f.whole (patch, "cueOutputChannels");

                for (const auto& [key, name] : f.archive.entries (f.at (patch, "cueOutputNames")))
                    if (const auto words = f.archive.text (name); words.has_value())
                        out.outputNames[std::atoi (key.c_str())] = *words;

                workspace.audioPatches.push_back (std::move (out));
            }

            //  QLab 4: patches by number, their outputs' names in a list beside them.
            const auto names = f.archive.items (f.entry (audio, "channelNames"));

            for (const auto& [key, patch] : f.archive.entries (f.entry (audio, "patches")))
            {
                AudioPatch out;
                out.id = key;
                out.name = f.archive.text (f.entry (patch, "name")).value_or (std::string {});

                const auto number = std::atoi (key.c_str());

                if (number >= 1 && static_cast<std::size_t> (number) <= names.size())
                    for (const auto& [output, name] : f.archive.entries (names[static_cast<std::size_t> (number - 1)]))
                        if (const auto words = f.archive.text (name); words.has_value())
                            out.outputNames[std::atoi (output.c_str())] = *words;

                workspace.audioPatches.push_back (std::move (out));
            }

            std::stable_sort (workspace.audioPatches.begin(), workspace.audioPatches.end(),
                       [] (const AudioPatch& a, const AudioPatch& b)
                       {
                           return std::atoi (a.id.c_str()) < std::atoi (b.id.c_str());
                       });
        }

        void readNetworkSettings (const Fields& f, Handle network, Handle osc, Workspace& workspace)
        {
            //  QLab 5: a patch a destination, by identifier.
            for (const auto patch : f.archive.items (f.entry (network, "networkPatches")))
            {
                const auto data = f.entry (patch, "data");
                NetworkPatch out;
                out.id = f.archive.text (f.entry (data, "uniqueID")).value_or (std::string {});
                out.name = f.archive.text (f.entry (data, "name")).value_or (std::string {});

                const auto device = f.archive.text (f.entry (data, "deviceIdentifier")).value_or (std::string {});
                out.kind = device == "com.figure53.oscmessage" ? "osc" : device;

                const auto states = f.archive.items (f.entry (data, "clientStates"));

                if (! states.empty())
                {
                    out.host = f.archive.text (f.entry (states.front(), "host")).value_or (std::string {});
                    out.port = static_cast<int> (f.archive.integer (f.entry (states.front(), "port")).value_or (0));
                    out.tcp = f.archive.boolean (f.entry (states.front(), "useTcp")).value_or (false);
                }

                workspace.networkPatches.push_back (std::move (out));
            }

            //  QLab 4: sixteen destinations by number, named beside them.
            const auto states = f.archive.items (f.entry (osc, "clientState"));
            const auto names = f.entry (osc, "names");

            for (std::size_t at = 0; at < states.size(); ++at)
            {
                NetworkPatch out;
                out.id = std::to_string (at + 1);
                out.name = f.archive.text (f.entry (names, out.id)).value_or (std::string {});
                out.kind = "osc";
                out.host = f.archive.text (f.entry (states[at], "host")).value_or (std::string {});
                out.port = static_cast<int> (f.archive.integer (f.entry (states[at], "port")).value_or (0));
                out.tcp = f.archive.boolean (f.entry (states[at], "useTcp")).value_or (false);

                if (! out.host.empty() || ! out.name.empty())
                    workspace.networkPatches.push_back (std::move (out));
            }
        }

        void count (const Cue& cue, int& n)
        {
            for (const auto& child : cue.children)
            {
                ++n;
                count (child, n);
            }
        }
    }

    //==========================================================================
    std::string FileRef::name() const
    {
        const auto& path = ! absolutePath.empty() ? absolutePath : relativePath;
        const auto slash = path.find_last_of ('/');
        return slash == std::string::npos ? path : path.substr (slash + 1);
    }

    int countCues (const Cue& list)
    {
        int n = 0;
        count (list, n);
        return n;
    }

    ReadResult readWorkspaceBytes (const std::uint8_t* bytes, std::size_t size)
    {
        ReadResult result;
        std::string error;

        const auto outer = archiveOf (bytes, size, error);

        if (! outer.has_value())
        {
            result.error = "not a QLab workspace: " + error;
            return result;
        }

        const Fields f { *outer };
        const auto root = outer->top();

        Workspace workspace;
        workspace.version = f.archive.text (f.entry (root, "QLabShortVersionString")).value_or (std::string {});
        workspace.name = f.archive.text (f.entry (root, "workspaceName")).value_or (std::string {});

        {
            const auto build = f.entry (root, "QLabBuildNumber");

            if (const auto words = f.archive.text (build); words.has_value())
                workspace.build = *words;
            else if (const auto number = f.archive.integer (build); number.has_value())
                workspace.build = std::to_string (*number);
        }

        if (workspace.version.empty())
        {
            result.error = "not a QLab workspace: it names no QLab version";
            return result;
        }

        workspace.major = std::atoi (workspace.version.c_str());

        /*  QLAB 4 AND 5, AND NOTHING ELSE (ZR), refused in words. */
        if (workspace.major != 4 && workspace.major != 5)
        {
            result.error = "a QLab " + workspace.version + " workspace: Go.dot reads QLab 4 and QLab 5 workspaces only";
            return result;
        }

        //  --- The settings ----------------------------------------------------
        const auto settings = f.entry (root, "settings");
        readAudioSettings (f, f.entry (settings, "Audio"), workspace);
        readNetworkSettings (f, f.entry (settings, "Network"), f.entry (settings, "OSC"), workspace);

        //  --- The cues: an archive held as data inside this one ----------------
        const auto* held = f.archive.bytes (f.entry (root, "cueLists"));

        if (held == nullptr)
        {
            result.error = "the workspace holds no cue lists";
            return result;
        }

        const auto inner = archiveOf (held->data(), held->size(), error);

        if (! inner.has_value())
        {
            result.error = "the workspace's cue lists could not be read: " + error;
            return result;
        }

        const Fields g { *inner };
        CueReader reader { g, workspace.major, workspace.problems, {} };
        const auto top = inner->top();
        reader.seen.insert (top);

        for (const auto list : inner->items (g.at (top, "cues")))
        {
            if (list == Archive::none || ! reader.seen.insert (list).second)
                continue;

            workspace.lists.push_back (reader.read (list, 0));
        }

        result.workspace = std::move (workspace);
        return result;
    }

    ReadResult readWorkspace (const juce::File& file)
    {
        juce::MemoryBlock bytes;

        if (! file.loadFileAsData (bytes))
        {
            ReadResult result;
            result.error = "the file could not be read: " + file.getFullPathName().toStdString();
            return result;
        }

        return readWorkspaceBytes (static_cast<const std::uint8_t*> (bytes.getData()), bytes.getSize());
    }
}
