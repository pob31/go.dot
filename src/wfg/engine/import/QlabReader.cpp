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

#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
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

        /*  NUMBERS WRITTEN AS TEXT, "0.5 0.25 1 1" - Cocoa's colour components,
            and its geometry strings "{{0, 0}, {1920, 1080}}" - read without the
            locale: a French machine reads 0.5, never 0. */
        std::vector<double> numbersIn (std::string_view text)
        {
            std::vector<double> numbers;
            std::string token;
            const auto flush = [&numbers, &token]
            {
                if (! token.empty())
                    numbers.push_back (juce::String (token).getDoubleValue());
                token.clear();
            };

            for (const char c : text)
            {
                if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E')
                    token += c;
                else
                    flush();
            }
            flush();
            return numbers;
        }

        std::string bytesAsText (const std::vector<std::uint8_t>* bytes)
        {
            return bytes == nullptr ? std::string {} : std::string (bytes->begin(), bytes->end());
        }

        /*  A COLOUR AS COCOA ARCHIVES IT: components with a colour space of
            their own, or RGB, a grey, CMYK, or a named colour's fallback. */
        Colour colourOf (const Fields& f, Handle colour, int depth = 0)
        {
            Colour out;
            if (colour == Archive::none || depth > 4)
                return out;

            const auto components = numbersIn (bytesAsText (f.archive.bytes (f.at (colour, "NSComponents"))));
            const auto rgb = numbersIn (bytesAsText (f.archive.bytes (f.at (colour, "NSRGB"))));
            const auto white = numbersIn (bytesAsText (f.archive.bytes (f.at (colour, "NSWhite"))));
            const auto cmyk = numbersIn (bytesAsText (f.archive.bytes (f.at (colour, "NSCMYK"))));

            const auto fromRgb = [&out] (const std::vector<double>& n)
            {
                out = { n[0], n[1], n[2], n.size() > 3 ? n[3] : 1.0, true };
            };
            const auto fromWhite = [&out] (const std::vector<double>& n)
            {
                out = { n[0], n[0], n[0], n.size() > 1 ? n[1] : 1.0, true };
            };

            if (rgb.size() >= 3)
                fromRgb (rgb);
            else if (components.size() >= 3)
                fromRgb (components);
            else if (! white.empty())
                fromWhite (white);
            else if (! components.empty())
                fromWhite (components);
            else if (cmyk.size() >= 4)
                out = { (1.0 - cmyk[0]) * (1.0 - cmyk[3]), (1.0 - cmyk[1]) * (1.0 - cmyk[3]),
                        (1.0 - cmyk[2]) * (1.0 - cmyk[3]), cmyk.size() > 4 ? cmyk[4] : 1.0, true };
            else
                return colourOf (f, f.at (colour, "NSColor"), depth + 1);    // a named colour's fallback

            return out;
        }

        /*  THE UTF-16 UNITS OF UTF-8 WORDS, as Cocoa counts a run's length:
            two for a character past the first plane. */
        std::size_t utf16Length (const std::string& words)
        {
            std::size_t units = 0;
            for (const char c : words)
            {
                const auto byte = static_cast<unsigned char> (c);
                if ((byte & 0xC0u) != 0x80u)
                    units += byte >= 0xF0u ? 2 : 1;
            }
            return units;
        }

        /*  ONE RUN'S ATTRIBUTES, from the dictionary Cocoa archives them in. */
        TextRun styleOf (const Fields& f, Handle attributes)
        {
            TextRun run;

            for (const auto& [name, value] : f.archive.entries (attributes))
            {
                if (name == "NSFont")
                {
                    run.font = f.text (value, "NSName");
                    run.size = f.number (value, "NSSize");
                }
                else if (name == "NSColor")
                    run.colour = colourOf (f, value);
                else if (name == "NSParagraphStyle")
                {
                    run.alignment = f.whole (value, "NSAlignment", -1);
                    run.lineHeightMultiple = f.number (value, "NSLineHeightMultiple");
                    run.lineSpacing = f.number (value, "NSLineSpacing");
                }
                else if (name == "NSStrokeWidth")
                    run.strokeWidth = f.archive.number (value).value_or (0.0);
                else if (name == "NSStrokeColor")
                    run.strokeColour = colourOf (f, value);
                else if (name == "NSShadow")
                {
                    run.shadow = true;
                    run.shadowBlur = f.number (value, "NSShadowBlurRadius");
                    run.shadowX = f.number (value, "NSShadowHoriz");
                    run.shadowY = f.number (value, "NSShadowVert");
                    run.shadowColour = colourOf (f, f.at (value, "NSShadowColor"));
                }
                else if (name == "NSBackgroundColor")
                    run.background = colourOf (f, value);
                else if (name == "NSUnderline")
                    run.underline = static_cast<int> (f.archive.integer (value).value_or (0));
                else if (name == "NSStrikethrough")
                    run.strikethrough = static_cast<int> (f.archive.integer (value).value_or (0));
                else if (name == "NSKern")
                    run.kern = f.archive.number (value).value_or (0.0);
            }
            return run;
        }

        /*  THE RUNS OF AN ATTRIBUTED STRING: one dictionary for the whole of
            it, or an array of dictionaries and `NSAttributeInfo` - pairs of
            numbers, each seven bits a byte, low first, the high bit saying
            another byte follows: a run's length, then which dictionary. */
        std::vector<TextRun> runsOf (const Fields& f, Handle attributed, const std::string& words)
        {
            std::vector<TextRun> runs;
            const auto attributes = f.at (attributed, "NSAttributes");
            const auto* info = f.archive.bytes (f.at (attributed, "NSAttributeInfo"));
            const auto length = utf16Length (words);

            if (info == nullptr)
            {
                if (attributes != Archive::none)
                {
                    auto run = styleOf (f, attributes);
                    run.length = length;
                    runs.push_back (std::move (run));
                }
                return runs;
            }

            const auto dictionaries = f.archive.items (attributes);
            std::vector<std::uint64_t> numbers;
            std::uint64_t value = 0;
            int shift = 0;

            for (const auto byte : *info)
            {
                if (shift < 63)
                    value |= static_cast<std::uint64_t> (byte & 0x7Fu) << shift;
                shift += 7;
                if ((byte & 0x80u) == 0)
                {
                    numbers.push_back (value);
                    value = 0;
                    shift = 0;
                }
            }

            std::size_t start = 0;
            for (std::size_t at = 0; at + 1 < numbers.size() && start < length; at += 2)
            {
                const auto which = static_cast<std::size_t> (numbers[at + 1]);
                auto run = which < dictionaries.size() ? styleOf (f, dictionaries[which]) : TextRun {};
                run.start = start;
                run.length = std::min<std::size_t> (static_cast<std::size_t> (numbers[at]), length - start);
                start += run.length;
                runs.push_back (std::move (run));
            }
            return runs;
        }

        /*  A QUATERNION, in whichever shape QLab keeps one: none is written
            `$null` in every workspace read so far; an array, a string or data
            of four numbers is read, anything else marked unread. */
        void quaternionOf (const Fields& f, Handle value, Placement& placement)
        {
            if (value == Archive::none)
                return;

            std::vector<double> numbers;
            for (const auto item : f.archive.items (value))
                if (const auto n = f.archive.number (item); n.has_value())
                    numbers.push_back (*n);

            if (numbers.empty())
                if (const auto words = f.archive.text (value); words.has_value())
                    numbers = numbersIn (*words);

            if (numbers.empty())
                if (const auto* bytes = f.archive.bytes (value); bytes != nullptr && (bytes->size() == 16 || bytes->size() == 32))
                {
                    const auto wide = bytes->size() == 32;
                    for (std::size_t at = 0; at < bytes->size(); at += wide ? 8 : 4)
                    {
                        if (wide)
                        {
                            double d = 0.0;
                            std::memcpy (&d, bytes->data() + at, 8);
                            numbers.push_back (d);
                        }
                        else
                        {
                            float x = 0.0f;
                            std::memcpy (&x, bytes->data() + at, 4);
                            numbers.push_back (static_cast<double> (x));
                        }
                    }
                }

            if (numbers.size() == 4)
                placement.quaternion = std::array<double, 4> { numbers[0], numbers[1], numbers[2], numbers[3] };
            else
                placement.quaternionUnread = true;
        }

        /*  WHERE A PICTURE GOES, or where a fade takes it: the same keys. */
        Placement placementOf (const Fields& f, Handle handle)
        {
            Placement p;
            p.translationX = f.number (handle, "translationX");
            p.translationY = f.number (handle, "translationY");
            p.scaleX = f.number (handle, "scaleX", 1.0);
            p.scaleY = f.number (handle, "scaleY", 1.0);
            p.originX = f.number (handle, "originX");
            p.originY = f.number (handle, "originY");
            p.rotation = f.number (handle, "rotation");
            p.opacity = f.number (handle, "opacity", 1.0);
            p.layer = f.number (handle, "layer", 1000.0);
            p.fullScreen = f.flag (handle, "fullScreen", true);
            p.fillStyle = f.whole (handle, "fillStyle");
            p.preserveAspectRatio = f.flag (handle, "preserveAspectRatio", true);
            p.cropLeft = f.number (handle, "cropLeft");
            p.cropRight = f.number (handle, "cropRight");
            p.cropTop = f.number (handle, "cropTop");
            p.cropBottom = f.number (handle, "cropBottom");

            //  The blend mode: an object holding its index in QLab 5, a number elsewhere.
            const auto blend = f.at (handle, "blendMode");
            if (const auto index = f.archive.integer (blend); index.has_value())
                p.blendMode = static_cast<int> (*index);
            else
                p.blendMode = f.whole (blend, "index");

            quaternionOf (f, f.at (handle, "quaternion"), p);
            return p;
        }

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
                else if (type == "Video" || type == "Titles" || type == "Camera")
                {
                    readPicture (handle, cue);
                }

                return cue;
            }

            /*  A VIDEO, TEXT OR CAMERA CUE (namespace draft §62): its stage, its
                place, its effects; a video cue's file and timing as an audio
                cue's, its sound from the audio cue QLab keeps inside it; a text
                cue's words and their runs. */
            void readPicture (Handle handle, Cue& cue)
            {
                cue.stage = major >= 5 ? f.text (handle, "viewportID") : f.idText (handle, "surfaceID");
                cue.placement = placementOf (f, handle);
                cue.holdLastFrame = f.flag (handle, "holdLastFrame");

                const auto effects = f.at (handle, "videoEffects");
                cue.hasEffects = f.flag (handle, "doEffect")
                              || ! f.archive.items (f.entry (effects, "effectEntries")).empty();

                //  QLab 5's last look at a screen drawn on directly: its name and its size.
                const auto lastSeen = f.at (handle, "lastSeenViewportInfo");
                if (lastSeen != Archive::none)
                {
                    cue.lastSeenScreen = f.archive.text (f.entry (lastSeen, "nameForMenuItem")).value_or (std::string {});
                    const auto bounds = numbersIn (f.archive.text (f.entry (lastSeen, "boundsOnStage")).value_or (std::string {}));
                    if (bounds.size() == 4)
                    {
                        cue.lastSeenWidth = static_cast<int> (bounds[2]);
                        cue.lastSeenHeight = static_cast<int> (bounds[3]);
                    }
                }

                if (cue.type == "Video")
                {
                    readAudio (handle, cue);

                    const auto sound = f.at (handle, "audioCue");
                    if (sound != Archive::none)
                    {
                        cue.levels = levelsOf (f.at (sound, "levels"));
                        cue.audioPatch = f.idText (sound, major >= 5 ? "audioOutputPatchID" : "patch");
                    }
                }
                else if (cue.type == "Titles")
                {
                    const auto attributed = f.at (handle, "titlesAttributedString");
                    cue.words = f.archive.text (attributed).value_or (std::string {});
                    cue.runs = runsOf (f, attributed, cue.words);
                    cue.fixedWidth = f.number (handle, "fixedWidth");

                    for (const auto name : f.archive.items (f.at (handle, "lastSeenFontNames")))
                        if (const auto words = f.archive.text (name); words.has_value())
                            cue.lastSeenFontNames.push_back (*words);
                }
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

                //  AND A PICTURE'S (§62): what it moves, and where to.
                cue.fadesOpacity = f.flag (handle, "doOpacity");
                cue.fadesTranslation = f.flag (handle, "doTranslation");
                cue.fadesScale = f.flag (handle, "doScale");
                cue.fadesRotation = f.flag (handle, "doRotation");
                cue.fadesEffects = ! f.archive.entries (f.at (handle, "doVideoEffectInputs")).empty();
                cue.geoMode = f.whole (handle, "geoMode");
                cue.rotationType = f.whole (handle, "rotationType");
                cue.placement = placementOf (f, handle);
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

        /*  THE STAGES (namespace draft §62.5): QLab 5's, each its viewport's UUID
            the id cues name, its outputs the routes of its regions; QLab 4's
            surfaces, each its number, its outputs the displays of its facets. */
        void readVideoSettings (const Fields& f, Handle video, Workspace& workspace)
        {
            for (const auto stage : f.archive.items (f.entry (video, "stages")))
            {
                Stage out;
                out.id = f.text (f.at (stage, "viewport"), "uniqueID");
                out.name = f.text (stage, "name");
                out.width = f.whole (stage, "width");
                out.height = f.whole (stage, "height");
                out.masked = f.at (f.at (stage, "viewport"), "maskAlias") != Archive::none;

                for (const auto region : f.archive.items (f.at (stage, "regions")))
                {
                    const auto route = f.text (f.at (region, "route"), "name");
                    if (! route.empty() && std::find (out.outputs.begin(), out.outputs.end(), route) == out.outputs.end())
                        out.outputs.push_back (route);

                    for (const auto* edge : { "edgeBlendLeftPixels", "edgeBlendRightPixels", "edgeBlendTopPixels", "edgeBlendBottomPixels" })
                        out.blended = out.blended || f.number (region, edge) > 0.0;
                }
                workspace.stages.push_back (std::move (out));
            }

            for (const auto surface : f.archive.items (f.entry (video, "surfaces")))
            {
                Stage out;
                out.id = f.idText (surface, "surfaceID");
                out.name = f.text (surface, "name");
                out.width = static_cast<int> (f.number (surface, "width"));
                out.height = static_cast<int> (f.number (surface, "height"));
                out.masked = f.at (surface, "maskImage") != Archive::none;

                for (const auto facet : f.archive.items (f.at (surface, "facets")))
                {
                    const auto display = f.text (f.at (facet, "display"), "name");
                    if (! display.empty() && std::find (out.outputs.begin(), out.outputs.end(), display) == out.outputs.end())
                        out.outputs.push_back (display);
                }
                workspace.stages.push_back (std::move (out));
            }

            const auto fallback = f.entry (video, "defaultSurfaceID");
            if (const auto number = f.archive.integer (fallback); number.has_value())
                workspace.defaultStage = std::to_string (*number);
            else
                workspace.defaultStage = f.archive.text (f.entry (video, "simpleStageKeyPath")).value_or (std::string {});
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
        readVideoSettings (f, f.entry (settings, "Video"), workspace);

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
