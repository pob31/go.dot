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

#include <juce_core/juce_core.h>

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

    using Fields = std::vector<wfg::test::plist::Entry>;

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

        //  --- Pictures and words (namespace draft §62) ---------------------------------

        /*  A COLOUR AS COCOA ARCHIVES ONE: in an RGB colour space, its
            components written as text with a nought after them, as Cocoa
            writes them - or a grey, `white` set. */
        std::int64_t colour (double r, double g, double b, double a = 1.0)
        {
            //  Written as Cocoa writes it whatever the locale: a point, never a comma.
            const auto text = (juce::String (r, 6) + " " + juce::String (g, 6) + " " + juce::String (b, 6) + " "
                               + juce::String (a, 6)).toStdString();
            std::vector<std::uint8_t> bytes (text.begin(), text.end());
            bytes.push_back (0);
            return cues.object ("NSColor", { { "NSColorSpace", Value::integer (1) }, { "NSRGB", Value::data (std::move (bytes)) } });
        }

        std::int64_t white (double w, double a = 1.0)
        {
            const auto text = (juce::String (w, 6) + " " + juce::String (a, 6)).toStdString();
            std::vector<std::uint8_t> bytes (text.begin(), text.end());
            bytes.push_back (0);
            return cues.object ("NSColor", { { "NSColorSpace", Value::integer (3) }, { "NSWhite", Value::data (std::move (bytes)) } });
        }

        /*  ONE RUN OF WORDS' STYLE: a font by its PostScript name and size, a
            colour, the paragraph's alignment and line height, and what else
            the Fonts panel gives - a stroke, a shadow, a background. A colour
            of -1 is none. */
        struct Run
        {
            std::size_t length = 0;
            std::string font = "HelveticaNeue-Bold";
            double size = 48.0;
            std::int64_t colour = -1;
            int alignment = 2;
            double lineHeight = 1.0;
            double strokeWidth = 0.0;
            std::int64_t strokeColour = -1;
            bool shadow = false;
            double shadowBlur = 0.0;
            std::int64_t shadowColour = -1;
            std::int64_t background = -1;
            int underline = 0;
        };

        std::int64_t attributes (const Run& run)
        {
            const auto key = [this] (const char* name) { return cues.add (Value::string (name)); };
            std::vector<std::pair<std::int64_t, std::int64_t>> entries;

            entries.push_back ({ key ("NSFont"), cues.object ("NSFont", { { "NSName", Value::uid (cues.string (run.font)) },
                                                                          { "NSSize", Value::number (run.size) },
                                                                          { "NSfFlags", Value::integer (16) } }) });
            entries.push_back ({ key ("NSParagraphStyle"),
                                 cues.object ("NSMutableParagraphStyle", { { "NSAlignment", Value::integer (run.alignment) },
                                                                           { "NSLineHeightMultiple", Value::number (run.lineHeight) } }) });
            if (run.colour >= 0)
                entries.push_back ({ key ("NSColor"), run.colour });
            if (std::abs (run.strokeWidth) > 0.0)     // GCC's -Wfloat-equal refuses != 0.0
            {
                entries.push_back ({ key ("NSStrokeWidth"), cues.add (Value::number (run.strokeWidth)) });
                if (run.strokeColour >= 0)
                    entries.push_back ({ key ("NSStrokeColor"), run.strokeColour });
            }
            if (run.shadow)
            {
                Fields shadow { { "NSShadowBlurRadius", Value::number (run.shadowBlur) },
                                { "NSShadowHoriz", Value::number (3.0) }, { "NSShadowVert", Value::number (-3.0) } };
                if (run.shadowColour >= 0)
                    shadow.push_back ({ "NSShadowColor", Value::uid (run.shadowColour) });
                entries.push_back ({ key ("NSShadow"), cues.object ("NSShadow", std::move (shadow)) });
            }
            if (run.background >= 0)
                entries.push_back ({ key ("NSBackgroundColor"), run.background });
            if (run.underline != 0)
                entries.push_back ({ key ("NSUnderline"), cues.add (Value::integer (run.underline)) });

            return cues.dictionary (entries);
        }

        /*  WORDS AND THEIR RUNS as Cocoa archives an attributed string: one
            run's attributes as a dictionary; several as an array, with
            `NSAttributeInfo` saying each run's length and which. */
        std::int64_t attributed (const std::string& words, const std::vector<Run>& runs)
        {
            Fields fields { { "NSString", Value::uid (cues.string (words)) } };

            if (runs.size() == 1)
                fields.push_back ({ "NSAttributes", Value::uid (attributes (runs.front())) });
            else
            {
                std::vector<std::int64_t> dictionaries;
                std::vector<std::uint8_t> info;
                const auto varint = [&info] (std::uint64_t n)
                {
                    do
                    {
                        auto byte = static_cast<std::uint8_t> (n & 0x7Fu);
                        n >>= 7;
                        if (n != 0)
                            byte |= 0x80u;
                        info.push_back (byte);
                    } while (n != 0);
                };

                for (std::size_t at = 0; at < runs.size(); ++at)
                {
                    dictionaries.push_back (attributes (runs[at]));
                    varint (runs[at].length);
                    varint (at);
                }
                fields.push_back ({ "NSAttributes", Value::uid (cues.array (dictionaries)) });
                fields.push_back ({ "NSAttributeInfo", Value::uid (cues.data (std::move (info))) });
            }

            return cues.object ("NSAttributedString", std::move (fields));
        }

        /*  A TEXT CUE: its words, on `stage` (QLab 5's viewport UUID, QLab 4's
            surface number), with any other fields - placement and the rest. */
        std::int64_t titles (const std::string& name, std::int64_t words, const std::string& stage, Fields more = {})
        {
            more.push_back ({ "titlesAttributedString", Value::uid (words) });
            more.push_back (stageField (stage));
            more.push_back ({ "lastSeenFontNames", Value::uid (cues.array ({ cues.string ("Helvetica Neue Bold") })) });
            return cue ("TitlesCue", name, std::move (more));
        }

        /*  A VIDEO CUE: its file as an audio cue's, its sound the audio cue QLab
            keeps inside it, on `stage`. */
        std::int64_t video (const std::string& name, const std::string& relativePath, const std::string& stage,
                            const std::vector<std::tuple<int, int, double>>& sound, Fields more = {})
        {
            Fields alias { { "lastKnownPath", Value::string ("/Volumes/Show/" + relativePath) } };

            if (major >= 5)
                alias.push_back ({ "relativePath", Value::string (relativePath) });
            else
                more.push_back ({ "relativePath", Value::string ("Show/" + relativePath) });

            more.push_back ({ "fileTarget", Value::uid (cues.object ("F53Alias", std::move (alias))) });
            more.push_back (stageField (stage));

            Fields audioFields { { "levels", Value::uid (levels (sound)) } };
            if (major >= 5)
                audioFields.push_back ({ "audioOutputPatchID", Value::string ("PATCH-AUDIO") });
            else
                audioFields.push_back ({ "patch", Value::integer (1) });
            more.push_back ({ "audioCue", Value::uid (cues.object ("AudioCue", std::move (audioFields))) });

            return cue ("VideoCue", name, std::move (more));
        }

        /*  A FADE OF A PICTURE: what it moves (doOpacity and the rest) and where
            to, as `picture` gives them. */
        std::int64_t pictureFade (const std::string& name, std::int64_t target, double duration, Fields picture,
                                  bool stop = false, int shapeType = 1)
        {
            const auto up = shape (shapeType, 1.0);
            Fields fadeFields { { "duration", Value::number (duration) } };
            if (major >= 5)
            {
                fadeFields.push_back ({ "fadeMode", Value::integer (1) });
                fadeFields.push_back ({ "shapes", Value::uid (cues.object ("FadeShapesFunction", { { "upShape", Value::uid (up) } })) });
            }
            else
            {
                fadeFields.push_back ({ "fadeType", Value::string ("absolute") });
                fadeFields.push_back ({ "upShape", Value::uid (up) });
            }

            picture.push_back ({ "cueTargetUniqueID", Value::string (idOf (target)) });
            picture.push_back ({ "duration", Value::number (duration) });
            picture.push_back ({ "stopTargetWhenDone", Value::boolean (stop) });
            picture.push_back ({ "fade", Value::uid (cues.object ("Fade", std::move (fadeFields))) });
            return cue ("FadeCue", name, std::move (picture));
        }

        /*  A STAGE (QLab 5) OR A SURFACE (QLab 4) for the settings: `id` what
            cues name, its size, the outputs it draws to. */
        struct StageSpec
        {
            std::string id;
            std::string name;
            int width = 1920;
            int height = 1080;
            std::vector<std::string> outputs;
            bool blended = false;
        };

        std::vector<StageSpec> stages;
        std::string screen;     ///< QLab 5's simple stage, where there are no stages

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

            //  The stages or surfaces, when the case gave any (§62).
            std::vector<std::pair<std::int64_t, std::int64_t>> video;
            if (major >= 5)
            {
                std::vector<std::int64_t> made5;
                for (const auto& stage : stages)
                {
                    std::vector<std::int64_t> regions;
                    for (const auto& output : stage.outputs)
                    {
                        const auto route = outer.object ("F53VideoOutputRoute", { { "name", Value::string (output) } });
                        regions.push_back (outer.object ("VideoStageRegion", { { "route", Value::uid (route) },
                                                                               { "edgeBlendLeftPixels", Value::number (stage.blended ? 120.0 : 0.0) },
                                                                               { "edgeBlendRightPixels", Value::number (0.0) } }));
                    }
                    const auto viewport = outer.object ("VideoStageViewport", { { "uniqueID", Value::string (stage.id) },
                                                                                { "name", Value::string ("main") } });
                    made5.push_back (outer.object ("VideoStage", { { "uniqueID", Value::string ("STAGE-" + stage.id) },
                                                                   { "name", Value::string (stage.name) },
                                                                   { "width", Value::integer (stage.width) },
                                                                   { "height", Value::integer (stage.height) },
                                                                   { "viewport", Value::uid (viewport) },
                                                                   { "regions", Value::uid (outer.array (regions)) } }));
                }
                video.push_back ({ key ("stages"), outer.array (made5) });
                video.push_back ({ key ("simpleStageKeyPath"), text (screen) });
            }
            else
            {
                std::vector<std::int64_t> made4;
                for (const auto& stage : stages)
                {
                    std::vector<std::int64_t> facets;
                    for (const auto& output : stage.outputs)
                        facets.push_back (outer.object ("F53VideoFacet", { { "display", Value::uid (outer.object ("F53DisplayAlias",
                                                                                   { { "name", Value::string (output) } })) } }));
                    made4.push_back (outer.object ("F53VideoSurface", { { "surfaceID", Value::integer (std::stoll (stage.id)) },
                                                                        { "name", Value::string (stage.name) },
                                                                        { "width", Value::number (stage.width) },
                                                                        { "height", Value::number (stage.height) },
                                                                        { "facets", Value::uid (outer.array (facets)) } }));
                }
                video.push_back ({ key ("surfaces"), outer.array (made4) });
                if (! stages.empty())
                    video.push_back ({ key ("defaultSurfaceID"), whole (std::stoll (stages.front().id)) });
            }

            const auto settings = outer.dictionary ({ { key ("Audio"), outer.dictionary (audio) },
                                                      { key (major >= 5 ? "Network" : "OSC"), outer.dictionary (network) },
                                                      { key ("Video"), outer.dictionary (video) } });

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

        wfg::test::plist::Entry stageField (const std::string& stage)
        {
            if (major >= 5)
                return { "viewportID", Value::string (stage) };
            return { "surfaceID", Value::integer (stage.empty() ? 0 : std::stoll (stage)) };
        }
    };
}
