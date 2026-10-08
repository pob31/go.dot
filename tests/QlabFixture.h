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

#pragma once

/*
    A QLAB WORKSPACE, BUILT FOR THE TESTS (namespace draft §46, QL.4-QL.6): the
    archive QLab 4 or QLab 5 writes, in the shape measured on the author's two
    shows (§46.5) - an outer archive of settings holding, as data, an inner one
    of cue lists - made from code, so every case says in a line what it puts
    in a workspace.

    INTERIM (ZQ): the probe workspaces the author makes in QLab are the
    fixtures that settle what QLab writes; until they arrive, this is what the
    measurement of 2026-10-08 says it writes, and nothing a real production
    holds.
*/

#include "PlistWriter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace wfg::test::qlab
{
    using wfg::test::plist::Keyed;
    using wfg::test::plist::Value;

    using Fields = std::vector<std::pair<std::string, Value>>;

    inline double gain (double db) { return db <= -150.0 ? 0.0 : std::pow (10.0, db / 20.0); }

    class Workspace
    {
    public:
        explicit Workspace (int majorToUse = 5) : major (majorToUse) {}

        //  --- Cues --------------------------------------------------------------

        /*  A CUE OF `className`, its UUID "Q-<n>" in the order made, its name and
            any other fields as given. Answers the UID; `idOf` its UUID. */
        std::int64_t cue (const std::string& className, const std::string& name, Fields more = {})
        {
            const auto id = "Q-" + std::to_string (++made);
            Fields fields { { "uniqueID", Value::string (id) }, { "armed", Value::boolean (true) },
                            { "preWait", Value::number (0.0) }, { "postWait", Value::number (0.0) },
                            { "continueMode", Value::integer (0) } };

            if (! name.empty())
                fields.push_back ({ "name", Value::uid (cues.string (name)) });

            /*  A FIELD GIVEN REPLACES THE DEFAULT, as QLab writes each key once. */
            for (auto& field : more)
            {
                const auto same = std::find_if (fields.begin(), fields.end(),
                                                [&field] (const auto& f) { return f.first == field.first; });

                if (same != fields.end())
                    same->second = std::move (field.second);
                else
                    fields.push_back (std::move (field));
            }

            const auto uid = cues.object (className, std::move (fields));
            ids.push_back ({ uid, id });
            return uid;
        }

        std::string idOf (std::int64_t uid) const
        {
            for (const auto& [u, id] : ids)
                if (u == uid)
                    return id;

            return {};
        }

        std::int64_t group (const std::string& name, int mode, const std::vector<std::int64_t>& members, Fields more = {})
        {
            more.push_back ({ "groupMode", Value::integer (mode) });
            more.push_back ({ "cues", Value::uid (cues.array (members)) });
            return cue ("GroupCue", name, std::move (more));
        }

        std::int64_t memo (const std::string& name, Fields more = {}) { return cue ("MemoCue", name, std::move (more)); }

        std::int64_t notes (std::int64_t uid, const std::string& words)
        {
            const auto note = cues.object ("NSAttributedString", { { "NSString", Value::uid (cues.string (words)) } });
            cues.at (uid).entries.push_back ({ "notes", Value::uid (note) });
            return uid;
        }

        std::int64_t set (std::int64_t uid, const std::string& key, Value value)
        {
            cues.at (uid).entries.push_back ({ key, std::move (value) });
            return uid;
        }

        /*  A LEVEL MATRIX: cells (row, column, dB), keyed as QLab keys them. */
        std::int64_t levels (const std::vector<std::tuple<int, int, double>>& cells)
        {
            std::vector<std::pair<std::int64_t, std::int64_t>> entries;

            for (const auto& [row, column, db] : cells)
            {
                const auto key = cues.add (Value::string (std::to_string (row * 1025 + column)));
                const auto knob = cues.object ("AudioLevelKnobs", { { "row", Value::integer (row) },
                                                                    { "column", Value::integer (column) },
                                                                    { "initialLevel", Value::number (gain (db)) },
                                                                    { "trimLevel", Value::number (1.0) } });
                entries.push_back ({ key, knob });
            }

            return cues.object ("AudioLevelMatrix", { { "rows", Value::integer (1025) },
                                                      { "columns", Value::integer (1025) },
                                                      { "entries", Value::uid (cues.dictionary (entries)) } });
        }

        std::int64_t audio (const std::string& name, const std::string& relativePath,
                            const std::vector<std::tuple<int, int, double>>& cells, Fields more = {})
        {
            Fields alias { { "lastKnownPath", Value::string ("/Volumes/Show/" + relativePath) } };

            if (major >= 5)
                alias.push_back ({ "relativePath", Value::string (relativePath) });
            else
                more.push_back ({ "relativePath", Value::string ("Show/" + relativePath) });

            more.push_back ({ "fileTarget", Value::uid (cues.object ("F53Alias", std::move (alias))) });
            more.push_back ({ "levels", Value::uid (levels (cells)) });

            if (major >= 5)
                more.push_back ({ "audioOutputPatchID", Value::string ("PATCH-AUDIO") });
            else
                more.push_back ({ "patch", Value::integer (1) });

            return cue ("AudioCue", name, std::move (more));
        }

        /*  A FADE: `target` the cue it fades, `cells` its faded cells (row,
            column, end dB - or the offset, for a relative one), `shape` QLab's
            shape type and parameter. */
        std::int64_t fade (const std::string& name, std::int64_t target, double duration, bool absolute,
                           const std::vector<std::tuple<int, int, double>>& cells, bool stop = false,
                           int shapeType = 1, double parameter = 1.0)
        {
            std::vector<std::pair<std::int64_t, std::int64_t>> entries;

            for (const auto& [row, column, db] : cells)
                entries.push_back ({ cues.add (Value::string (std::to_string (row * 1025 + column))),
                                     cues.object ("FadeValueEntry", { { "row", Value::integer (row) },
                                                                      { "column", Value::integer (column) },
                                                                      { "startValue", Value::number (1.0e-4) },
                                                                      { "endValue", Value::number (gain (db)) } }) });

            const auto up = shape (shapeType, parameter);
            Fields fadeFields { { "entries", Value::uid (cues.dictionary (entries)) },
                                { "duration", Value::number (duration) } };

            if (major >= 5)
            {
                fadeFields.push_back ({ "fadeMode", Value::integer (absolute ? 1 : 0) });
                fadeFields.push_back ({ "fadeType", Value::integer (1) });
                fadeFields.push_back ({ "shapes", Value::uid (cues.object ("FadeShapesFunction", { { "upShape", Value::uid (up) } })) });
            }
            else
            {
                fadeFields.push_back ({ "fadeType", Value::string (absolute ? "absolute" : "relative") });
                fadeFields.push_back ({ "upShape", Value::uid (up) });
            }

            return cue ("FadeCue", name, { { "cueTargetUniqueID", Value::string (idOf (target)) },
                                           { "duration", Value::number (duration) },
                                           { "stopTargetWhenDone", Value::boolean (stop) },
                                           { "fade", Value::uid (cues.object ("Fade", std::move (fadeFields))) } });
        }

        std::int64_t shape (int type, double parameter)
        {
            const auto a = cues.object ("FadeShapeEntry", { { "t", Value::number (0.0) }, { "v", Value::number (0.0) } });
            const auto b = cues.object ("FadeShapeEntry", { { "t", Value::number (1.0) }, { "v", Value::number (1.0) } });
            return cues.object ("FadeShape", { { "type", Value::integer (type) },
                                               { "curveParameter", Value::number (parameter) },
                                               { "shapeEntries", Value::uid (cues.array ({ a, b })) } });
        }

        /*  A NETWORK CUE: its message in QLab's text, through `patch` (a QLab 5
            UUID or a QLab 4 number); `fadeFrom`/`fadeTo` make it a `#v#` fade. */
        std::int64_t network (const std::string& name, const std::string& message, const std::string& patch,
                              Fields more = {})
        {
            if (major >= 5)
            {
                more.push_back ({ "oscString", Value::string (message) });
                more.push_back ({ "networkPatchID", Value::string (patch) });
            }
            else
            {
                more.push_back ({ "rawString", Value::string (message) });
                more.push_back ({ "messageType", Value::integer (2) });
                more.push_back ({ "patch", Value::integer (std::stoi (patch)) });
            }

            return cue ("OSCCue", name, std::move (more));
        }

        std::int64_t networkFade (const std::string& name, const std::string& message, const std::string& patch,
                                  double from, double to, double duration, int shapeType = 3)
        {
            const auto up = shape (shapeType, 1.0);
            const auto fadeObject = major >= 5
                ? cues.object ("Fade", { { "shapes", Value::uid (cues.object ("FadeShapesFunction", { { "upShape", Value::uid (up) } })) } })
                : cues.object ("Fade", { { "upShape", Value::uid (up) } });

            return network (name, message, patch, { { "fadeType", Value::integer (1) },
                                                    { "startValue", Value::number (from) },
                                                    { "endValue", Value::number (to) },
                                                    { "duration", Value::number (duration) },
                                                    { "fadingFloats", Value::boolean (true) },
                                                    { "fade", Value::uid (fadeObject) } });
        }

        //  --- The workspace -------------------------------------------------------

        /*  THE FILE: the lists given, under a root group, archived as data
            inside the outer archive with the settings - an audio patch of four
            named outputs, and network patches WFS and S21. */
        std::vector<std::uint8_t> bytes (const std::vector<std::int64_t>& lists, double minVolume = -80.0)
        {
            const auto root = cues.object ("GroupCue", { { "groupMode", Value::integer (3) },
                                                         { "cues", Value::uid (cues.array (lists)) } });
            const auto inner = cues.archive (root);

            Keyed outer;
            const auto key = [&outer] (const std::string& text) { return outer.add (Value::string (text)); };
            const auto text = [&outer] (const std::string& words) { return outer.add (Value::string (words)); };
            const auto number = [&outer] (double value) { return outer.add (Value::number (value)); };
            const auto whole = [&outer] (std::int64_t value) { return outer.add (Value::integer (value)); };

            std::vector<std::pair<std::int64_t, std::int64_t>> audio, network;

            if (major >= 5)
            {
                const auto names = outer.dictionary ({ { key ("1"), text ("FOH L") }, { key ("2"), text ("FOH R") },
                                                       { key ("3"), text ("Mon 1") }, { key ("4"), text ("Mon 2") } });
                const auto patch = outer.object ("AudioOutputPatch", { { "uniqueID", Value::string ("PATCH-AUDIO") },
                                                                       { "name", Value::string ("Desk") },
                                                                       { "cueOutputChannels", Value::integer (4) },
                                                                       { "cueOutputNames", Value::uid (names) } });
                audio.push_back ({ key ("audioOutputPatches"), outer.array ({ patch }) });

                const auto destination = [&] (const std::string& id, const std::string& name, const std::string& host, int port)
                {
                    const auto state = outer.dictionary ({ { key ("host"), text (host) }, { key ("port"), whole (port) },
                                                           { key ("useTcp"), outer.add (Value::boolean (false)) } });
                    const auto data = outer.dictionary ({ { key ("uniqueID"), text (id) }, { key ("name"), text (name) },
                                                          { key ("deviceIdentifier"), text ("com.figure53.oscmessage") },
                                                          { key ("clientStates"), outer.array ({ state }) } });
                    return outer.dictionary ({ { key ("data"), data } });
                };

                network.push_back ({ key ("networkPatches"),
                                     outer.array ({ destination ("PATCH-WFS", "WFS", "192.168.1.32", 8051),
                                                    destination ("PATCH-S21", "S21", "192.168.1.221", 8024) }) });
            }
            else
            {
                const auto patch = outer.dictionary ({ { key ("name"), text ("Desk") } });
                audio.push_back ({ key ("patches"), outer.dictionary ({ { key ("1"), patch } }) });
                audio.push_back ({ key ("channelNames"),
                                   outer.array ({ outer.dictionary ({ { key ("1"), text ("FOH L") }, { key ("2"), text ("FOH R") } }) }) });

                const auto state = [&] (const std::string& host, int port)
                {
                    return outer.dictionary ({ { key ("host"), text (host) }, { key ("port"), whole (port) },
                                               { key ("useTcp"), outer.add (Value::boolean (false)) } });
                };

                network.push_back ({ key ("clientState"), outer.array ({ state ("localhost", 53000),
                                                                         state ("192.168.1.32", 8051),
                                                                         state ("192.168.1.221", 8024),
                                                                         state ("", 0) }) });
                network.push_back ({ key ("names"), outer.dictionary ({ { key ("1"), text ("localhost") },
                                                                        { key ("2"), text ("WFS") },
                                                                        { key ("3"), text ("S21") } }) });
            }

            audio.push_back ({ key ("minVolume"), number (minVolume) });

            const auto settings = outer.dictionary ({ { key ("Audio"), outer.dictionary (audio) },
                                                      { key (major >= 5 ? "Network" : "OSC"), outer.dictionary (network) } });

            const auto top = outer.dictionary ({
                { key ("QLabShortVersionString"), text (major >= 5 ? "5.6.3" : "4.4.5") },
                { key ("QLabBuildNumber"), major >= 5 ? whole (5603) : text ("4405") },
                { key ("workspaceName"), text ("Fixture") },
                { key ("settings"), settings },
                { key ("cueLists"), outer.data (inner) } });

            return outer.archive (top);
        }

        Keyed cues;

    private:
        int major = 5;
        int made = 0;
        std::vector<std::pair<std::int64_t, std::string>> ids;
    };
}
