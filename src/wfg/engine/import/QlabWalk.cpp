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

#include <wfg/engine/import/QlabWalk.h>

#include <wfg/engine/osc/OscValue.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>

namespace wfg::import::qlab
{
    namespace
    {
        constexpr double silence = -120.0;
        constexpr double tiny = 0.0005;
        constexpr double pi = 3.14159265358979323846;

        /*  A FILE GO.DOT SHOWS AS A STILL (§62, ALH); anything else is a movie. */
        bool isStill (const std::string& name)
        {
            const auto dot = name.find_last_of ('.');
            if (dot == std::string::npos)
                return false;

            std::string extension;
            for (const char c : name.substr (dot + 1))
                extension += static_cast<char> (std::tolower (static_cast<unsigned char> (c)));

            static const std::set<std::string> stills { "png", "jpg", "jpeg", "gif", "tif", "tiff", "bmp", "psd",
                                                        "heic", "heif", "webp", "tga", "exr" };
            return stills.count (extension) != 0;
        }

        /*  A TEXT CUE'S WORDS WITH THEIR BRACES DOUBLED (§62, ALI): Go.dot reads
            `{name}` as a field (§61, AKM), QLab as the words it is. */
        std::string bracesKept (const std::string& words)
        {
            std::string out;
            for (const char c : words)
            {
                out += c;
                if (c == '{' || c == '}')
                    out += c;
            }
            return out;
        }

        std::string hexOf (const Colour& colour)
        {
            const auto byte = [] (double v) { return static_cast<int> (std::lround (std::clamp (v, 0.0, 1.0) * 255.0)); };
            static const char* digits = "0123456789ABCDEF";
            std::string out = "#";
            for (const auto v : { byte (colour.r), byte (colour.g), byte (colour.b) })
            {
                out += digits[(v >> 4) & 0xF];
                out += digits[v & 0xF];
            }
            return out;
        }

        bool sameLook (const TextRun& a, const TextRun& b)
        {
            const auto colour = [] (const Colour& x, const Colour& y)
            {
                return x.set == y.set && std::abs (x.r - y.r) < 0.002 && std::abs (x.g - y.g) < 0.002
                    && std::abs (x.b - y.b) < 0.002 && std::abs (x.a - y.a) < 0.002;
            };
            return a.font == b.font && std::abs (a.size - b.size) < 0.01 && colour (a.colour, b.colour);
        }

        /*  A FAMILY FROM A FONT'S NAME, where this machine has not got it (ALI):
            the name QLab last saw, "Helvetica Neue Bold", less its style words;
            or the PostScript name, "HelveticaNeue-Bold", its parts spaced. */
        FontFace faceFromName (const std::string& seen, const std::string& postScript)
        {
            static const std::vector<std::string> boldWords { "Bold", "Semibold", "SemiBold", "Demibold", "DemiBold",
                                                              "Black", "Heavy", "ExtraBold", "UltraBold" };
            static const std::vector<std::string> italicWords { "Italic", "Oblique" };
            static const std::vector<std::string> otherWords { "Regular", "Roman", "Book", "Light", "Medium", "Thin",
                                                               "UltraLight", "ExtraLight", "Condensed", "Narrow", "Plain" };
            FontFace face;

            const auto styleWord = [&] (const std::string& word)
            {
                const auto in = [&word] (const std::vector<std::string>& words)
                {
                    return std::find (words.begin(), words.end(), word) != words.end();
                };
                face.bold = face.bold || in (boldWords);
                face.italic = face.italic || in (italicWords);
                return in (boldWords) || in (italicWords) || in (otherWords);
            };

            if (! seen.empty() && seen.find (' ') != std::string::npos)
            {
                std::vector<std::string> words;
                std::string word;
                for (const char c : seen + " ")
                {
                    if (c == ' ')
                    {
                        if (! word.empty())
                            words.push_back (word);
                        word.clear();
                    }
                    else
                        word += c;
                }
                while (words.size() > 1 && styleWord (words.back()))
                    words.pop_back();
                for (std::size_t at = 0; at < words.size(); ++at)
                    face.family += (at == 0 ? "" : " ") + words[at];
                return face;
            }

            const auto dash = postScript.find ('-');
            const auto family = postScript.substr (0, dash);
            if (dash != std::string::npos)
            {
                //  "BoldItalic", "BoldOblique": each capitalised word of the style.
                std::string word;
                for (const char c : postScript.substr (dash + 1) + "A")
                {
                    if (std::isupper (static_cast<unsigned char> (c)) && ! word.empty())
                    {
                        styleWord (word);
                        word.clear();
                    }
                    word += c;
                }
            }

            for (std::size_t at = 0; at < family.size(); ++at)
            {
                const auto c = family[at];
                if (at > 0 && std::isupper (static_cast<unsigned char> (c)) && std::islower (static_cast<unsigned char> (family[at - 1])))
                    face.family += ' ';
                face.family += c;
            }
            return face;
        }

        bool isWhole (const std::string& text)
        {
            const auto start = text.size() > 1 && text[0] == '-' ? 1u : 0u;
            return start < text.size()
                && std::all_of (text.begin() + start, text.end(), [] (char c) { return c >= '0' && c <= '9'; });
        }

        bool isDecimal (const std::string& text)
        {
            const auto start = text.size() > 1 && text[0] == '-' ? 1u : 0u;
            const auto point = text.find ('.', start);

            if (point == std::string::npos || text.find ('.', point + 1) != std::string::npos)
                return false;

            const auto digits = std::count_if (text.begin() + start, text.end(), [] (char c) { return c >= '0' && c <= '9'; });
            return digits > 0 && static_cast<std::size_t> (digits) == text.size() - start - 1;
        }

        /*  QLAB'S S-CURVE, "ease-in, ease-out", sampled as a raised cosine at
            nine points: the nearest a curve of straight segments comes. */
        std::vector<std::pair<double, double>> eased (double from, double to, double duration)
        {
            std::vector<std::pair<double, double>> points;

            for (int step = 0; step <= 8; ++step)
            {
                const auto t = step / 8.0;
                const auto done = 0.5 - 0.5 * std::cos (t * 3.14159265358979323846);
                points.push_back ({ std::round (t * duration * 1.0e4) / 1.0e4,
                                    std::round ((from + (to - from) * done) * 1.0e6) / 1.0e6 });
            }

            return points;
        }

        void setRow (Attributes& rows, const std::string& name, const std::string& value)
        {
            for (auto& row : rows)
                if (row.first == name)
                {
                    row.second = value;
                    return;
                }

            rows.push_back ({ name, value });
        }

        //======================================================================
        struct Walker
        {
            const Workspace& workspace;
            const WalkOptions& options;
            Plan plan;

            std::map<std::string, const Cue*> byId;
            std::map<std::string, std::string> kindOf;     ///< what each imported cue becomes
            std::map<std::string, double> trims;           ///< a group's trim, summed in show order (ZV)
            std::map<std::string, std::string> segmentOwner;
            std::map<int, std::string> channelOwner;
            int list = -1;

            //  --- Saying what was done ----------------------------------------

            void note (Note::Kind kind, const Cue& cue, const std::string& text)
            {
                plan.notes.push_back ({ kind, list, where (cue), text });
            }

            std::string where (const Cue& cue) const
            {
                return (cue.number.empty() ? std::string {} : cue.number + " ") + displayName (cue);
            }

            /*  QLAB'S DISPLAYED NAME where nobody typed one (ZY): a sound's file,
                a fade's "fade" and its target, a message's text. */
            std::string displayName (const Cue& cue) const
            {
                if (! cue.name.empty())
                    return cue.name;

                const auto target = [this, &cue]
                {
                    const auto found = byId.find (cue.target);
                    return found == byId.end() ? std::string {} : " " + displayName (*found->second);
                };

                if (cue.type == "Audio" || cue.type == "Video")
                    return cue.file.name().empty() ? cue.type : cue.file.name();

                if (cue.type == "Fade")
                    return (cue.stopTargetWhenDone ? "fade and stop" : "fade") + target();

                if (cue.type == "Start" || cue.type == "Stop" || cue.type == "Goto" || cue.type == "Arm"
                    || cue.type == "Disarm" || cue.type == "Devamp")
                {
                    auto verb = cue.type;
                    verb[0] = static_cast<char> (std::tolower (static_cast<unsigned char> (verb[0])));
                    return verb + target();
                }

                if (cue.type == "OSC" && ! cue.message.empty())
                    return cue.message;

                return cue.type;
            }

            //  --- Pass one: what each cue becomes --------------------------------

            void index (const Cue& cue)
            {
                byId[cue.id] = &cue;

                for (const auto& child : cue.children)
                    index (child);
            }

            static std::string kindFor (const Cue& cue)
            {
                static const std::map<std::string, std::string> kinds {
                    { "Group", "group" }, { "Audio", "media" }, { "Mic", "mic" }, { "Fade", "fade" },
                    { "Video", "video" }, { "Titles", "video" },
                    { "OSC", "osc" }, { "Start", "start" }, { "Stop", "transport" }, { "Goto", "transport" },
                    { "Arm", "transport" }, { "Disarm", "transport" }, { "Devamp", "transport" },
                    { "Memo", "memo" }, { "Wait", "memo" } };

                const auto found = kinds.find (cue.type);
                return found == kinds.end() ? std::string {} : found->second;
            }

            void classify (const Cue& cue)
            {
                kindOf[cue.id] = kindFor (cue);

                for (const auto& child : cue.children)
                    classify (child);
            }

            /*  WHAT A TARGET IS IN THE SHOW: its kind, empty when it is not
                imported - in a list not ticked, or a memo in its place. */
            std::string targetKind (const std::string& id) const
            {
                const auto found = kindOf.find (id);
                return found == kindOf.end() ? std::string {} : found->second;
            }

            //  --- Pass two: the items ---------------------------------------------

            Attributes common (const Cue& cue) const
            {
                Attributes rows;

                if (! cue.number.empty())
                    rows.push_back ({ "number", cue.number });

                if (! cue.notes.empty())
                    rows.push_back ({ "notes", cue.notes });

                if (cue.preWait > tiny)
                    rows.push_back ({ "preWait", number (cue.preWait, 4) });

                if (cue.postWait > tiny)
                    rows.push_back ({ "postWait", number (cue.postWait, 4) });

                if (! cue.armed)
                    rows.push_back ({ "enabled", "false" });

                return rows;
            }

            /*  A MEMO IN THE PLACE OF A CUE GO.DOT CANNOT CARRY (ZP), the original
                in its notes: one GO in QLab is still one GO here. */
            Item placeholder (const Cue& cue, const std::string& why, const std::string& original)
            {
                ++plan.placeholders;
                note (Note::Kind::dropped, cue, why + " - a memo stands in its place");

                Item item;
                item.kind = "memo";
                item.key = "cue:" + cue.id;
                item.name = "[QLab] " + displayName (cue);
                item.attributes = common (cue);

                auto words = "Not imported: " + why + ". QLab " + cue.type + " cue"
                             + (original.empty() ? std::string {} : ": " + original) + ".";

                if (! cue.notes.empty())
                    words += "\n\n" + cue.notes;

                setRow (item.attributes, "notes", words);
                kindOf[cue.id] = {};
                return item;
            }

            void triggers (const Cue& cue)
            {
                std::vector<std::string> had;

                if (cue.hotkeyTrigger)     had.push_back ("a hotkey");
                if (cue.midiTrigger)       had.push_back ("a MIDI trigger");
                if (cue.wallClockTrigger)  had.push_back ("a wall-clock trigger");
                if (cue.timecodeTrigger)   had.push_back ("a timecode trigger");

                if (had.empty())
                    return;

                std::string words;

                for (std::size_t at = 0; at < had.size(); ++at)
                    words += (at == 0 ? "" : at + 1 == had.size() ? " and " : ", ") + had[at];

                note (Note::Kind::dropped, cue, "QLab fired it by " + words + ": not imported (§46.2, ZY)");
            }

            Item build (const Cue& cue)
            {
                ++plan.cues;
                triggers (cue);

                if (cue.type == "Group")   return group (cue);
                if (cue.type == "Audio")   return media (cue);
                if (cue.type == "Fade")    return fade (cue);
                if (cue.type == "OSC")     return message (cue);
                if (cue.type == "Mic")     return mic (cue);
                if (cue.type == "Memo" || cue.type == "Wait") return memo (cue);

                if (cue.type == "Start" || cue.type == "Stop" || cue.type == "Goto" || cue.type == "Arm"
                    || cue.type == "Disarm" || cue.type == "Devamp")
                    return control (cue);

                if (cue.type == "Script")
                    return placeholder (cue, "a script is never run (PRD §3.20)", cue.source);

                if (cue.type == "Video")   return picture (cue);
                if (cue.type == "Titles")  return text (cue);
                if (cue.type == "Camera")  return placeholder (cue, "a camera cue stays a memo (§62, AKT)", {});

                return placeholder (cue, "Go.dot has no " + cue.type + " cue (§46.4)", {});
            }

            //  --- Pictures and words (namespace draft §62, ALF-ALK) --------------------
            //  Provisional where the probe (§62.6) is to settle a unit, a sign or a number:
            //  QLab's Y up and its turn counter-clockwise, its fill styles 0 fit 1 fill 2
            //  stretch, a point a stage pixel, Cocoa's alignments 0 left 1 right 2 centre,
            //  a fade's geometry mode 1 relative.

            std::map<std::string, std::size_t> canvasAt;
            std::set<std::string> fontsSaid;

            Canvas canvas (const std::string& key, const std::string& name, int width, int height,
                           const std::vector<std::string>& outputs)
            {
                if (const auto found = canvasAt.find (key); found != canvasAt.end())
                    return plan.canvases[found->second];

                canvasAt[key] = plan.canvases.size();
                plan.canvases.push_back ({ key, name, std::clamp (width, 16, 16384), std::clamp (height, 16, 16384), outputs });
                return plan.canvases.back();
            }

            /*  THE CANVAS A PICTURE IS ON (ALF): its stage, or QLab 4's default
                surface where it names none the workspace has; QLab 5 drawing on
                a screen directly, a canvas for that screen of the size QLab last
                saw it. */
            Canvas canvasFor (const Cue& cue)
            {
                const auto stageNamed = [this] (const std::string& id) -> const Stage*
                {
                    if (id.empty())
                        return nullptr;
                    for (const auto& stage : workspace.stages)
                        if (stage.id == id)
                            return &stage;
                    return nullptr;
                };

                const auto* stage = stageNamed (cue.stage);
                if (stage == nullptr)
                    stage = stageNamed (workspace.defaultStage);

                if (stage != nullptr)
                    return canvas ("stage:" + stage->id, stage->name.empty() ? std::string ("Stage") : stage->name,
                                   stage->width, stage->height, stage->outputs);

                const auto name = cue.lastSeenScreen.empty() ? std::string ("Screen") : cue.lastSeenScreen;
                const auto known = cue.lastSeenWidth > 0 && cue.lastSeenHeight > 0;
                if (! known && canvasAt.count ("screen:" + name) == 0)
                    note (Note::Kind::approximated, cue, "the stage it is on is not in the workspace: it is put on a "
                          "canvas of 1920 by 1080 called " + name);
                return canvas ("screen:" + name, name, known ? cue.lastSeenWidth : 1920, known ? cue.lastSeenHeight : 1080,
                               { name });
            }

            /*  HOW MUCH LARGER GO.DOT'S `scale` IS THAN QLAB'S (ALG): a picture
                QLab drew at its own size is scaled from what fits the canvas, so
                its own size over the fitted one; a picture filling its stage, and
                a text, one. Nought when its own size is not known. */
            double scaleFactorOf (const Cue& cue, const Canvas& on) const
            {
                if (cue.type == "Titles" || cue.placement.fullScreen)
                    return 1.0;

                const auto size = options.pictureSizes.find (cue.id);
                if (size == options.pictureSizes.end() || size->second.first <= 0 || size->second.second <= 0)
                    return 0.0;

                const auto fits = std::min (static_cast<double> (on.width) / size->second.first,
                                            static_cast<double> (on.height) / size->second.second);
                return fits > 0.0 ? 1.0 / fits : 0.0;
            }

            //  A turn about the screen's axis, from QLab's quaternion: counter-clockwise there, clockwise here.
            static double turnOf (const std::array<double, 4>& q)
            {
                return -2.0 * std::atan2 (q[2], q[3]) * 180.0 / pi;
            }

            static bool tilted (const std::array<double, 4>& q)
            {
                return std::abs (q[0]) > 1.0e-4 || std::abs (q[1]) > 1.0e-4;
            }

            /*  WHERE IT GOES (ALG): QLab's pixels a % of the canvas, its scale,
                turn, opacity, layer; what Go.dot has not got said. `opacityTimes`
                is a text colour's own alpha, multiplied in. */
            void place (const Cue& cue, const Canvas& on, Item& item, double opacityTimes)
            {
                const auto& p = cue.placement;
                const auto words = cue.type == "Titles";

                if (std::abs (p.translationX) > tiny)
                    item.attributes.push_back ({ "offsetX", number (p.translationX / on.width * 100.0, 4) });
                if (std::abs (p.translationY) > tiny)
                    item.attributes.push_back ({ "offsetY", number (p.translationY / on.height * 100.0, 4) });

                if (! words && p.fullScreen)
                {
                    const std::string fit = ! p.preserveAspectRatio ? "stretch"
                                          : p.fillStyle == 1 ? "fill" : p.fillStyle == 2 ? "stretch" : "fit";
                    if (fit != "fit")
                        item.attributes.push_back ({ "fit", fit });
                }

                const auto factor = scaleFactorOf (cue, on);
                if (factor <= 0.0)
                    note (Note::Kind::approximated, cue, "QLab drew it at its own size, which is not known here - its file "
                          "was not read: it is fitted to its canvas, then scaled as QLab scaled it");

                const auto scale = p.scaleX * 100.0 * (factor > 0.0 ? factor : 1.0);
                if (std::abs (scale - 100.0) > 0.005)
                    item.attributes.push_back ({ "scale", number (std::clamp (scale, 0.0, 1000.0), 4) });

                if (std::abs (p.scaleX - p.scaleY) > tiny)
                    note (Note::Kind::approximated, cue, "QLab scaled it " + number (p.scaleX * 100.0, 1) + " % across and "
                          + number (p.scaleY * 100.0, 1) + " % up and down: Go.dot scales both ways alike, as across");

                if (p.quaternion.has_value())
                {
                    if (const auto turn = turnOf (*p.quaternion); std::abs (turn) > 0.005)
                        item.attributes.push_back ({ "rotation", number (turn, 3) });
                    if (tilted (*p.quaternion))
                        note (Note::Kind::approximated, cue, "QLab tilted it in depth: only its turn about the screen's axis "
                              "is imported");
                }
                if (p.quaternionUnread)
                    note (Note::Kind::dropped, cue, "QLab kept its turn in a form Go.dot does not read: not imported");

                const auto opacity = std::clamp (p.opacity, 0.0, 1.0) * std::clamp (opacityTimes, 0.0, 1.0);
                if (opacity < 1.0 - tiny)
                    item.attributes.push_back ({ "opacity", number (opacity * 100.0, 2) });

                if (const auto layer = static_cast<int> (std::clamp (std::round (p.layer), 0.0, 999.0)); layer != 0)
                    item.attributes.push_back ({ "layer", std::to_string (layer) });

                if (p.blendMode != 0)
                    note (Note::Kind::approximated, cue, "QLab's blend mode " + std::to_string (p.blendMode) + " is drawn "
                          "normally: which mode QLab's number is waits for the probe workspace (§62.6)");
                if (std::abs (p.originX) > tiny || std::abs (p.originY) > tiny)
                    note (Note::Kind::approximated, cue, "QLab moved its origin: it is placed by its middle, and a fade turns "
                          "or scales it about its middle");
                if (p.cropLeft > tiny || p.cropRight > tiny || p.cropTop > tiny || p.cropBottom > tiny)
                    note (Note::Kind::dropped, cue, "QLab cropped it: Go.dot has no crop, so it is shown whole");
                if (cue.hasEffects)
                    note (Note::Kind::dropped, cue, "its video effects are not imported");
            }

            /*  A VIDEO CUE (ALH): a still or a movie on its canvas, its file
                found and copied as a sound's, a movie's timing its Ranges. */
            Item picture (const Cue& cue)
            {
                Item item;
                item.kind = "video";
                item.key = "cue:" + cue.id;
                item.cueId = cue.id;
                item.name = displayName (cue);
                item.file = cue.file;
                item.attributes = common (cue);

                const auto on = canvasFor (cue);
                item.canvas = on.key;

                const auto still = isStill (cue.file.name());
                item.attributes.push_back ({ "source", still ? "picture" : "movie" });

                if (! still)
                {
                    if (std::abs (cue.rate - 1.0) > tiny)
                        item.attributes.push_back ({ "rate", number (std::clamp (cue.rate, -20.0, 20.0), 4) });
                    ranges (cue, item);

                    if (cue.holdLastFrame)
                        note (Note::Kind::approximated, cue, "QLab held its last frame: a Go.dot movie ends with its last "
                              "pass, its picture going with it");

                    const auto main = std::find_if (cue.levels.begin(), cue.levels.end(),
                                                    [] (const Level& l) { return l.row == 0 && l.column == 0; });
                    if (main != cue.levels.end() && decibels (main->gain, workspace.minVolume) > silence)
                        note (Note::Kind::info, cue, "its sound played in QLab, at " + number (decibels (main->gain, workspace.minVolume), 2)
                              + " dB: Go.dot plays a movie's sound from the sound cue its conversion to HAP makes beside it, "
                              "locked to it - set that cue's level there");
                }

                place (cue, on, item, 1.0);
                kindOf[cue.id] = "video";
                return item;
            }

            /*  ITS FONT (ALI): the family this machine has under QLab's
                PostScript name, else the family read from the name QLab saw -
                each named once in the report (ALK). */
            void font (const Cue& cue, const TextRun& run, Item& item)
            {
                if (run.font.empty())
                    return;

                std::optional<FontFace> face;
                if (options.fontOf)
                    face = options.fontOf (run.font);

                FontUse use { run.font, {}, face.has_value() ? "installed" : "missing" };
                if (! face.has_value())
                {
                    const auto seen = cue.lastSeenFontNames.size() == 1 && cue.runs.size() <= 1
                                    ? cue.lastSeenFontNames.front() : std::string {};
                    face = faceFromName (seen, run.font);
                    note (Note::Kind::approximated, cue, "its font " + run.font + " is not installed on this machine: its "
                          "family is taken to be " + face->family + " - carry the font in the show from a machine that has it");
                }
                use.family = face->family;

                if (fontsSaid.insert (run.font).second)
                    plan.fonts.push_back (use);

                item.attributes.push_back ({ "font", face->family });
                if (face->bold)
                    item.attributes.push_back ({ "bold", "true" });
                if (face->italic)
                    item.attributes.push_back ({ "italic", "true" });
            }

            /*  A TEXT CUE (ALI): its words, braces doubled; its first run's look
                for all of them (AKU); the size in the canvas's height, the box
                QLab's fixed width or none; the stroke the outline, a shadow a glow. */
            Item text (const Cue& cue)
            {
                Item item;
                item.kind = "video";
                item.key = "cue:" + cue.id;
                item.name = displayName (cue);
                item.attributes = common (cue);

                const auto on = canvasFor (cue);
                item.canvas = on.key;
                item.attributes.push_back ({ "source", "text" });
                item.attributes.push_back ({ "words", bracesKept (cue.words) });

                const auto run = cue.runs.empty() ? TextRun {} : cue.runs.front();
                font (cue, run, item);

                if (run.size > 0.0)
                    item.attributes.push_back ({ "fontSize", number (std::clamp (run.size / on.height * 100.0, 0.1, 100.0), 4) });

                //  Cocoa's own colour where none is said: black.
                const auto colour = run.colour.set ? run.colour : Colour { 0.0, 0.0, 0.0, 1.0, true };
                item.attributes.push_back ({ "paint", hexOf (colour) });

                const std::string align = run.alignment == 1 ? "right" : run.alignment == 2 ? "centre" : "left";
                if (align != "centre")
                    item.attributes.push_back ({ "align", align });
                if (run.alignment == 3)
                    note (Note::Kind::approximated, cue, "its lines are justified in QLab: aligned left here");

                if (run.lineHeightMultiple > 0.0 && std::abs (run.lineHeightMultiple - 1.0) > tiny)
                    item.attributes.push_back ({ "lineSpacing", number (std::clamp (run.lineHeightMultiple * 100.0, 50.0, 400.0), 2) });
                if (run.lineSpacing > tiny)
                    note (Note::Kind::approximated, cue, "QLab adds " + number (run.lineSpacing, 1) + " points between its "
                          "lines: not imported");

                item.attributes.push_back ({ "wrap", number (cue.fixedWidth > 0.0 ? std::clamp (cue.fixedWidth / on.width * 100.0, 0.0, 100.0) : 0.0, 4) });

                if (std::abs (run.strokeWidth) > tiny)
                {
                    //  Cocoa strokes the letters' edge both ways, by a % of the size: half of it is outside.
                    item.attributes.push_back ({ "outline", number (std::clamp (std::abs (run.strokeWidth) / 100.0 * run.size / 2.0, 0.0, 200.0), 2) });
                    item.attributes.push_back ({ "outlineColour", hexOf (run.strokeColour.set ? run.strokeColour : colour) });
                    if (run.strokeWidth > 0.0)
                        note (Note::Kind::approximated, cue, "QLab drew its letters hollow, the outline alone: here they are "
                              "filled, inside their outline");
                }
                else if (run.shadow)
                {
                    const auto shade = run.shadowColour.set ? run.shadowColour : Colour { 0.0, 0.0, 0.0, 1.0 / 3.0, true };
                    item.attributes.push_back ({ "outline", "1" });
                    item.attributes.push_back ({ "outlineBlur", number (std::clamp (run.shadowBlur, 0.0, 200.0), 2) });
                    item.attributes.push_back ({ "outlineColour", hexOf (shade) });
                    note (Note::Kind::approximated, cue, "QLab's shadow is drawn as a glow about the letters: its offset of "
                          + number (run.shadowX, 1) + ", " + number (run.shadowY, 1) + " points is not imported");
                }

                if (run.background.set)
                    note (Note::Kind::dropped, cue, "its background colour is not imported");
                if (run.underline != 0 || run.strikethrough != 0)
                    note (Note::Kind::dropped, cue, "its underline or strikethrough is not imported");
                if (std::abs (run.kern) > tiny)
                    note (Note::Kind::dropped, cue, "its letter spacing is not imported");

                const auto changes = std::count_if (cue.runs.begin() + (cue.runs.empty() ? 0 : 1), cue.runs.end(),
                                                    [&run] (const TextRun& r) { return ! sameLook (r, run); });
                if (changes > 0)
                    note (Note::Kind::approximated, cue, "its words change font, size or colour in QLab: the first run's look "
                          "is used for all of them (AKU)");

                place (cue, on, item, colour.a);
                kindOf[cue.id] = "video";
                return item;
            }

            /*  A FADE OF A PICTURE OR A TEXT (ALJ): the fade's `video` pairs,
                where QLab's switches say it moves something - a relative move
                added to where the picture was put. */
            Item pictureFade (const Cue& cue, const Cue& aimed)
            {
                const auto on = canvasFor (aimed);
                const auto factor = scaleFactorOf (aimed, on);
                const auto relative = cue.geoMode == 1;
                const auto& p = cue.placement;
                const auto& from = aimed.placement;

                std::vector<std::pair<std::string, std::string>> pairs;

                if (cue.fadesOpacity)
                    pairs.push_back ({ "opacity", number (std::clamp (p.opacity, 0.0, 1.0) * 100.0, 2) });

                if (cue.fadesTranslation)
                {
                    const auto x = relative ? from.translationX + p.translationX : p.translationX;
                    const auto y = relative ? from.translationY + p.translationY : p.translationY;
                    pairs.push_back ({ "offsetX", number (x / on.width * 100.0, 4) });
                    pairs.push_back ({ "offsetY", number (y / on.height * 100.0, 4) });
                }

                if (cue.fadesScale)
                {
                    const auto s = relative ? from.scaleX * p.scaleX : p.scaleX;
                    pairs.push_back ({ "scale", number (std::clamp (s * 100.0 * (factor > 0.0 ? factor : 1.0), 0.0, 1000.0), 4) });
                }

                if (cue.fadesRotation)
                {
                    double turn = p.quaternion.has_value() ? turnOf (*p.quaternion) : -p.rotation;
                    if (relative && from.quaternion.has_value())
                        turn += turnOf (*from.quaternion);
                    pairs.push_back ({ "rotation", number (turn, 3) });
                }

                const auto sound = std::any_of (cue.fadeLevels.begin(), cue.fadeLevels.end(),
                                                [] (const FadeLevel& f) { return f.row == 0 && f.column == 0; });

                if (pairs.empty() && sound)
                    return placeholder (cue, "it fades only the movie's sound, which in Go.dot is the sound cue the movie's "
                                        "conversion to HAP makes - a fade of that cue", displayName (cue));

                if (cue.fadesEffects)
                    note (Note::Kind::dropped, cue, "it fades video effects too: not imported");
                if (sound)
                    note (Note::Kind::dropped, cue, "it fades the movie's sound too: a Go.dot movie's sound is the sound cue "
                          "its conversion to HAP makes, which a fade of its own moves");

                if (pairs.empty())
                    return placeholder (cue, "it moves nothing a Go.dot fade of a picture moves", displayName (cue));

                if (relative && (cue.fadesTranslation || cue.fadesScale || cue.fadesRotation))
                    note (Note::Kind::approximated, cue, "a relative move in QLab, added to where the picture was put: right "
                          "when the show runs in order");

                std::sort (pairs.begin(), pairs.end());
                std::string video;
                for (const auto& [name, value] : pairs)
                    video += (video.empty() ? "" : " ") + name + ":" + value;

                Item item;
                item.kind = "fade";
                item.key = "cue:" + cue.id;
                item.name = displayName (cue);
                item.attributes = common (cue);
                item.target = "cue:" + cue.target;
                item.attributes.push_back ({ "duration", number (std::max (0.0, cue.duration), 4) });
                item.attributes.push_back ({ "video", video });

                if (cue.stopTargetWhenDone)
                    item.attributes.push_back ({ "stopWhenDone", "true" });

                if (cue.shape.type != 3)
                    item.attributes.push_back ({ "curve", "sCurve" });
                if (cue.shape.type != 1 && cue.shape.type != 3)
                    note (Note::Kind::approximated, cue, "a shape drawn in QLab, taken as Go.dot's S-curve");

                return item;
            }

            //  --- Groups and chains (ZS) --------------------------------------------

            std::vector<Item> members (const Cue& group)
            {
                std::vector<Item> out;
                const auto& children = group.children;

                /*  CONTINUE MODES SOMEBODY SET mean something only where the
                    group runs its members one by one: in a timeline QLab calls
                    them broken, and a playlist's are QLab's own. */
                const auto chains = group.groupMode != 3 && group.groupMode != 6;

                for (std::size_t at = 0; at < children.size();)
                {
                    if (! chains || children[at].continueMode == 0)
                    {
                        if (! chains && children[at].continueMode != 0 && group.groupMode == 3)
                            note (Note::Kind::info, children[at], "its continue mode is ignored: a timeline starts all its members");

                        out.push_back (build (children[at]));
                        ++at;
                        continue;
                    }

                    auto end = at;

                    while (end + 1 < children.size() && children[end].continueMode != 0)
                        ++end;

                    out.push_back (chain (children, at, end));
                    at = end + 1;
                }

                return out;
            }

            /*  A CHAIN somebody set - each cue continuing into the next - made a
                group of its own (ZS): all auto-follow, an automatic sequence; all
                auto-continue, a timeline whose offsets are the waits; mixed, an
                automatic sequence, the auto-continues taken as follows. */
            Item chain (const std::vector<Cue>& children, std::size_t from, std::size_t to)
            {
                const auto& first = children[from];

                bool follows = true, continues = true;

                for (auto at = from; at < to; ++at)
                {
                    follows = follows && children[at].continueMode == 2;
                    continues = continues && children[at].continueMode == 1;
                }

                Item group;
                group.kind = "group";
                group.key = "chain:" + first.id;
                group.name = "Chain from " + where (first);

                if (continues)
                {
                    group.attributes.push_back ({ "mode", "timeline" });

                    double trigger = 0.0;

                    for (auto at = from; at <= to; ++at)
                    {
                        auto item = build (children[at]);
                        const auto offset = trigger + children[at].preWait;

                        if (offset > tiny)
                            setRow (item.attributes, "preWait", number (offset, 4));

                        trigger += children[at].preWait + children[at].postWait;
                        group.children.push_back (std::move (item));
                    }
                }
                else
                {
                    group.attributes.push_back ({ "mode", "sequence" });
                    group.attributes.push_back ({ "advance", "auto" });

                    for (auto at = from; at <= to; ++at)
                        group.children.push_back (build (children[at]));
                }

                note (continues || follows ? Note::Kind::info : Note::Kind::approximated, first,
                      continues ? "a chain of auto-continues, made a timeline group by the importer"
                      : follows ? "a chain of auto-follows, made an automatic group by the importer"
                                : "a chain of auto-follows and auto-continues, made an automatic group: each "
                                  "auto-continue now waits for the cue before it to finish");
                return group;
            }

            Item group (const Cue& cue)
            {
                Item item;
                item.kind = "group";
                item.key = "cue:" + cue.id;
                item.name = displayName (cue);
                item.attributes = common (cue);

                switch (cue.groupMode)
                {
                    case 1:
                        item.attributes.push_back ({ "mode", "sequence" });
                        item.attributes.push_back ({ "advance", "manual" });
                        break;

                    case 2:
                        item.attributes.push_back ({ "mode", "sequence" });

                        if (cue.children.size() > 1)
                        {
                            /*  QLAB FIRES THE FIRST AND MOVES PAST THE GROUP: the
                                rest run only when somebody fires them. Manual
                                keeps a GO between them; automatic would fire them
                                unasked (§46.2, ZS - the author's "REALLY ?"). */
                            item.attributes.push_back ({ "advance", "manual" });
                            note (Note::Kind::approximated, cue, "start first with several members: QLab fires the "
                                  "first only and moves on, so each of the others needs its own GO here");
                        }
                        else
                        {
                            item.attributes.push_back ({ "advance", "auto" });
                        }
                        break;

                    case 3:
                        item.attributes.push_back ({ "mode", "timeline" });
                        break;

                    case 4:
                        item.attributes.push_back ({ "mode", "sequence" });
                        item.attributes.push_back ({ "advance", "auto" });
                        item.attributes.push_back ({ "selection", "shuffle" });
                        item.attributes.push_back ({ "play", "1" });
                        note (Note::Kind::approximated, cue, "start random: one member a GO, drawn from a shuffled "
                              "round - never the same one twice across a round's end, where QLab may");
                        break;

                    case 6:
                        item.attributes.push_back ({ "mode", "sequence" });
                        item.attributes.push_back ({ "advance", "auto" });

                        if (cue.playlistLoop)
                            item.attributes.push_back ({ "loops", "0" });

                        if (cue.playlistShuffle)
                            item.attributes.push_back ({ "selection", "shuffle" });

                        /*  AND ITS CROSSFADE, as the sequence's join (namespace
                            draft §69, AOI): equal power across its length, which
                            QLab's own curve only approximates; two seconds where
                            the workspace says no length. */
                        if (cue.playlistCrossfade)
                        {
                            const auto known = cue.playlistCrossfadeDuration > 0.0;
                            const auto seconds = known ? std::max (0.1, cue.playlistCrossfadeDuration) : 2.0;

                            item.attributes.push_back ({ "join", "crossfade" });
                            item.attributes.push_back ({ "joinLength", osc::formatDouble (seconds) });
                            note (Note::Kind::approximated, cue,
                                  known ? "the playlist's crossfade is the sequence's crossfade join, at its length, "
                                          "Go.dot's equal-power shape (§69)"
                                        : "the playlist's crossfade is the sequence's crossfade join; the workspace "
                                          "gave no length, so two seconds (§69)");
                        }
                        break;

                    default:
                        item.attributes.push_back ({ "mode", "sequence" });
                        note (Note::Kind::approximated, cue, "a group of QLab mode " + std::to_string (cue.groupMode)
                              + ", imported as a manual sequence");
                        break;
                }

                item.children = members (cue);
                return item;
            }

            //  --- Sounds (ZT, ZU) ----------------------------------------------------

            const AudioPatch* audioPatch (const std::string& id) const
            {
                for (const auto& patch : workspace.audioPatches)
                    if (patch.id == id)
                        return &patch;

                return nullptr;
            }

            /*  THE BUS FOR A CUE OUTPUT, made the first time an output is used,
                on the interface's channel of the same number. */
            std::string busFor (const std::string& patchId, int output, const Cue& cue)
            {
                const auto key = "out:" + patchId + ":" + std::to_string (output);

                if (std::none_of (plan.buses.begin(), plan.buses.end(), [&key] (const Bus& b) { return b.key == key; }))
                {
                    const auto* patch = audioPatch (patchId);
                    Bus bus;
                    bus.key = key;

                    if (patch != nullptr && patch->outputNames.count (output) != 0)
                        bus.name = patch->outputNames.at (output);
                    else
                        bus.name = (patch != nullptr && ! patch->name.empty() ? patch->name : std::string ("Output")) + " "
                                   + std::to_string (output);

                    if (channelOwner.count (output) == 0)
                    {
                        channelOwner[output] = key;
                        bus.channel = output - 1;
                    }
                    else
                    {
                        note (Note::Kind::approximated, cue, "a second audio patch's output " + std::to_string (output)
                              + " has no interface channel of its own: its bus is left unpatched");
                    }

                    plan.buses.push_back (std::move (bus));
                }

                return key;
            }

            Item media (const Cue& cue)
            {
                Item item;
                item.kind = "media";
                item.key = "cue:" + cue.id;
                item.cueId = cue.id;
                item.name = displayName (cue);
                item.file = cue.file;
                item.attributes = common (cue);

                std::map<std::pair<int, int>, double> cell;

                for (const auto& level : cue.levels)
                    cell[{ level.row, level.column }] = level.gain;

                const auto gainAt = [&cell] (int row, int column, double otherwise)
                {
                    const auto found = cell.find ({ row, column });
                    return found == cell.end() ? otherwise : found->second;
                };

                const auto floor = workspace.minVolume;

                if (const auto main = decibels (gainAt (0, 0, 1.0), floor); std::abs (main) > 0.005)
                    item.attributes.push_back ({ "level", number (main, 2) });

                if (std::abs (cue.rate - 1.0) > tiny)
                {
                    item.attributes.push_back ({ "rate", number (std::clamp (cue.rate, 0.0, 20.0), 4) });

                    if (! cue.pitchFollowsRate)
                        item.attributes.push_back ({ "rateMode", "timestretch" });
                }

                ranges (cue, item);

                //  --- Where it goes -------------------------------------------------
                const auto channels = options.channels.find (cue.id);
                const auto crosspoints = std::any_of (cue.levels.begin(), cue.levels.end(),
                                                      [] (const Level& l) { return l.row >= 1 && l.column >= 1; });

                if (channels == options.channels.end() || channels->second <= 0)
                {
                    note (Note::Kind::dropped, cue, "its file was not found, so how many channels it has - and so its "
                          "routing - is not known: it is written with no route");
                }
                else if (! crosspoints)
                {
                    item.defaultRoute = true;
                    note (Note::Kind::approximated, cue, "QLab's routing was left as it was, which the workspace does not "
                          "store: it is given Go.dot's default route");
                }
                else
                {
                    std::set<int> outputs;

                    for (const auto& level : cue.levels)
                        if (level.row >= 1 && level.row <= channels->second && level.column >= 1)
                            outputs.insert (level.column);

                    for (const auto output : outputs)
                    {
                        std::vector<double> gains;
                        bool heard = false;

                        for (int row = 1; row <= channels->second; ++row)
                        {
                            const auto gain = gainAt (0, output, 1.0) * gainAt (row, 0, 1.0) * gainAt (row, output, 0.0);
                            const auto db = decibels (gain, floor);
                            const auto written = db <= silence ? 0.0 : std::round (std::pow (10.0, db / 20.0) * 1.0e6) / 1.0e6;
                            gains.push_back (written);
                            heard = heard || written > 0.0;
                        }

                        if (heard)
                            item.routes.push_back ({ busFor (cue.audioPatch, output, cue), std::move (gains) });
                    }
                }

                kindOf[cue.id] = "media";
                return item;
            }

            /*  THE FILE'S RANGES (ZT): the slices, each with its play count, or one
                range from the start to the end with the cue's loops - none where
                the cue plays its whole file once. */
            void ranges (const Cue& cue, Item& item)
            {
                const auto end = cue.endTime > tiny ? cue.endTime : cue.fileDuration;
                const auto loopsOf = [] (int playCount, bool infinite) { return infinite ? 0 : std::max (1, playCount); };

                if (! cue.slices.empty())
                {
                    if (end <= cue.startTime + tiny)
                    {
                        note (Note::Kind::dropped, cue, "its slices are not imported: the workspace does not say where it ends");
                        return;
                    }

                    auto slices = cue.slices;
                    std::sort (slices.begin(), slices.end(), [] (const Slice& a, const Slice& b) { return a.time < b.time; });

                    double from = cue.startTime;

                    for (const auto& slice : slices)
                    {
                        if (slice.time <= from + tiny || slice.time >= end - tiny)
                            continue;

                        item.ranges.push_back ({ from, slice.time, loopsOf (slice.playCount, slice.infinite) });
                        from = slice.time;
                    }

                    item.ranges.push_back ({ from, end, loopsOf (cue.lastSlice.playCount, cue.lastSlice.infinite) });

                    if (cue.infiniteLoop || cue.playCount > 1)
                        note (Note::Kind::approximated, cue, "the whole cue's loop over its slices is not imported: each "
                              "slice keeps its own");
                    return;
                }

                const auto loops = cue.infiniteLoop || cue.playCount > 1;
                const auto cut = cue.startTime > tiny || (cue.fileDuration > tiny && end < cue.fileDuration - tiny);

                if (! loops && ! cut)
                    return;

                if (end <= cue.startTime + tiny)
                {
                    if (cue.startTime > tiny)
                        item.attributes.push_back ({ "startOffset", number (cue.startTime, 4) });

                    if (loops)
                        note (Note::Kind::dropped, cue, "its loop is not imported: the workspace does not say where it ends");
                    return;
                }

                item.ranges.push_back ({ cue.startTime, end, loopsOf (cue.playCount, cue.infiniteLoop) });
            }

            Item mic (const Cue& cue)
            {
                Item item;
                item.kind = "mic";
                item.key = "cue:" + cue.id;
                item.name = displayName (cue);
                item.attributes = common (cue);
                note (Note::Kind::approximated, cue, "a live input with no input or rack channel assigned: QLab took "
                      + std::to_string (cue.inputChannels) + " channel(s) from input " + std::to_string (cue.inputChannel + 1)
                      + "; which of the show's inputs it is, is the designer's to say (PRD §3.18)");
                return item;
            }

            Item memo (const Cue& cue)
            {
                Item item;
                item.kind = "memo";
                item.key = "cue:" + cue.id;
                item.name = displayName (cue);
                item.attributes = common (cue);

                /*  A WAIT IS ITS PRE-WAIT: a memo that is done once its time has
                    passed. */
                if (cue.type == "Wait" && cue.duration > tiny)
                    setRow (item.attributes, "preWait", number (cue.preWait + cue.duration, 4));

                return item;
            }

            //  --- Fades (ZV) ---------------------------------------------------------

            Item fade (const Cue& cue)
            {
                const auto target = targetKind (cue.target);
                const auto found = byId.find (cue.target);

                if (found == byId.end())
                    return placeholder (cue, "its target is not imported", displayName (cue));

                const auto& aimed = *found->second;

                if (target.empty())
                    return placeholder (cue, "its target, a " + aimed.type + " cue, is not imported", displayName (cue));

                const auto main = std::find_if (cue.fadeLevels.begin(), cue.fadeLevels.end(),
                                                [] (const FadeLevel& f) { return f.row == 0 && f.column == 0; });
                const auto others = cue.fadeLevels.size() - (main == cue.fadeLevels.end() ? 0u : 1u);

                if (target == "video")
                    return pictureFade (cue, aimed);

                if (target != "media" && target != "mic" && target != "group")
                    return placeholder (cue, "it fades a " + aimed.type + " cue", displayName (cue));

                if (main == cue.fadeLevels.end() && ! cue.fadesRate)
                    return placeholder (cue, "it fades output or crosspoint levels only, which a Go.dot fade does not move",
                                        displayName (cue));

                Item item;
                item.kind = "fade";
                item.key = "cue:" + cue.id;
                item.name = displayName (cue);
                item.attributes = common (cue);
                item.target = "cue:" + cue.target;

                item.attributes.push_back ({ "duration", number (std::max (0.0, cue.duration), 4) });

                if (others > 0)
                    note (Note::Kind::approximated, cue, "it also fades " + std::to_string (others) + " output or "
                          "crosspoint level(s), which a Go.dot fade does not move: only the main level is imported");

                if (main != cue.fadeLevels.end())
                {
                    double level = 0.0;

                    if (cue.absolute)
                    {
                        level = decibels (main->end, workspace.minVolume);

                        if (target == "group")
                            note (Note::Kind::approximated, cue, "an absolute fade of a group, imported as the group's "
                                  "trim (decision O): a master over its members rather than each member's level");
                    }
                    else if (target == "group")
                    {
                        /*  A RELATIVE FADE OF A GROUP IS ITS TRIM, the offsets
                            summed in show order - right when the show runs in
                            order (ZV). */
                        const auto offset = main->end <= 0.0 ? silence : 20.0 * std::log10 (main->end);
                        auto& trim = trims[cue.target];
                        trim = offset <= silence ? silence : std::clamp (trim + offset, silence, 12.0);
                        level = trim;
                        note (Note::Kind::approximated, cue, (offset <= silence ? std::string ("a relative fade to silence")
                                                                                : "a relative fade of " + number (offset, 2) + " dB")
                              + " on a group, imported as its trim to " + (level <= silence ? std::string ("silence")
                                                                                               : number (level, 2) + " dB")
                              + " - the offsets summed in show order");
                    }
                    else
                    {
                        return placeholder (cue, "a relative fade on one cue: a Go.dot fade is absolute, and the level it "
                                            "starts from is not known before the show runs", displayName (cue));
                    }

                    item.attributes.push_back ({ "level", number (std::clamp (level, silence, 12.0), 2) });
                }
                else
                {
                    item.attributes.push_back ({ "levelOn", "false" });
                }

                if (cue.fadesRate)
                {
                    item.attributes.push_back ({ "rateOn", "true" });
                    item.attributes.push_back ({ "rate", number (std::clamp (cue.rate, 0.0, 20.0), 4) });
                }

                if (cue.stopTargetWhenDone)
                    item.attributes.push_back ({ "stopWhenDone", "true" });

                switch (cue.shape.type)
                {
                    case 1:
                        item.attributes.push_back ({ "curve", "sCurve" });
                        break;

                    case 3:
                        break;

                    case 2:
                        item.attributes.push_back ({ "curve", "sCurve" });
                        note (Note::Kind::approximated, cue, "QLab's parametric shape (intensity " + number (cue.shape.parameter, 2)
                              + ") taken as Go.dot's S-curve");
                        break;

                    default:
                        item.attributes.push_back ({ "curve", "sCurve" });
                        note (Note::Kind::approximated, cue, "a shape drawn in QLab, taken as Go.dot's S-curve");
                        break;
                }

                return item;
            }

            //  --- Messages and devices (ZW) ----------------------------------------------

            const NetworkPatch* networkPatch (const std::string& id) const
            {
                for (const auto& patch : workspace.networkPatches)
                    if (patch.id == id)
                        return &patch;

                return nullptr;
            }

            /*  THE DEVICE FOR A PATCH, made the first time a cue sends through it,
                each address's first segment one of its prefixes - unless another
                device claimed it first, which is said. */
            void deviceFor (const NetworkPatch& patch, const std::string& address, const Cue& cue)
            {
                const auto key = "patch:" + patch.id;
                auto found = std::find_if (plan.devices.begin(), plan.devices.end(), [&key] (const Device& d) { return d.key == key; });

                if (found == plan.devices.end())
                {
                    Device device;
                    device.key = key;
                    device.name = patch.name;
                    device.host = patch.host.empty() || patch.host == "localhost" ? "127.0.0.1" : patch.host;
                    device.port = patch.port;
                    device.tcp = patch.tcp;
                    plan.devices.push_back (std::move (device));
                    found = plan.devices.end() - 1;
                }

                const auto slash = address.find ('/', 1);
                const auto segment = slash == std::string::npos ? address : address.substr (0, slash);

                if (const auto owner = segmentOwner.find (segment); owner != segmentOwner.end())
                {
                    if (owner->second != key)
                        note (Note::Kind::approximated, cue, "two network patches send under " + segment + ": its messages "
                              "go to the device that claimed it first");
                    return;
                }

                segmentOwner[segment] = key;
                found->prefix += (found->prefix.empty() ? "" : " ") + segment;
            }

            Item message (const Cue& cue)
            {
                const auto* patch = networkPatch (cue.networkPatch);
                const auto words = cue.message;

                if (patch == nullptr)
                    return placeholder (cue, "its network patch is not in the workspace", words);

                if (patch->kind != "osc")
                    return placeholder (cue, "its patch is not an OSC message patch (" + patch->kind + ")", words);

                if (workspace.major < 5 && cue.messageType != 2)
                    return placeholder (cue, cue.messageType == 1 ? "a QLab command, not an OSC message" : "a UDP message, not OSC",
                                        words);

                if ((patch->host == "localhost" || patch->host == "127.0.0.1") && patch->port == 53000)
                    return placeholder (cue, "it is sent to QLab itself", words);

                const auto parts = splitMessage (words);

                if (parts.empty() || parts.front().empty() || parts.front()[0] != '/')
                    return placeholder (cue, "its message has no OSC address", words);

                if (parts.front().find ('#') != std::string::npos)
                    return placeholder (cue, "a value in the address, which a Go.dot curve does not move", words);

                if (parts.front() == "/godot" || parts.front().rfind ("/godot/", 0) == 0)
                    return placeholder (cue, "an address under Go.dot's own /godot", words);

                Item item;
                item.kind = "osc";
                item.key = "cue:" + cue.id;
                item.name = displayName (cue);
                item.attributes = common (cue);

                std::vector<std::string> atoms;
                int faded = -1;

                const auto valueAtom = [&cue] (double v)
                {
                    return cue.fadeFloats ? osc::Value::float32 (static_cast<float> (v)).toAtom()
                                          : osc::Value::int32 (static_cast<std::int32_t> (std::lround (v))).toAtom();
                };

                const auto joined = [&atoms]
                {
                    std::string out;

                    for (const auto& atom : atoms)
                        out += (out.empty() ? "" : " ") + atom;

                    return out;
                };

                for (std::size_t at = 1; at < parts.size(); ++at)
                {
                    std::string atom;

                    if (parts[at] == "#v#")
                    {
                        faded = static_cast<int> (at - 1);
                        atom = valueAtom (cue.fadeFrom);
                    }
                    else if (parts[at].find ('#') != std::string::npos
                             && (parts[at].find ("#x#") != std::string::npos || parts[at].find ("#y#") != std::string::npos
                                 || parts[at].find ("#v#") != std::string::npos))
                    {
                        return placeholder (cue, cue.networkFadeType == 2 ? "a 2D path, not imported yet"
                                                                          : "a placeholder inside an argument", words);
                    }
                    else
                    {
                        atom = atomFor (parts[at]);
                    }

                    atoms.push_back (atom);
                }

                item.attributes.push_back ({ "address", parts.front() });

                if (! atoms.empty())
                    item.attributes.push_back ({ "value", joined() });

                if (faded >= 0 && cue.networkFadeType == 1)
                {
                    if (cue.duration <= tiny)
                    {
                        atoms[static_cast<std::size_t> (faded)] = valueAtom (cue.fadeTo);
                        setRow (item.attributes, "value", joined());
                        note (Note::Kind::approximated, cue, "a fade of no duration: its last value is sent");
                    }
                    else
                    {
                        Curve curve;
                        curve.arg = faded;

                        if (cue.shape.type == 3)
                        {
                            curve.points = { { 0.0, cue.fadeFrom }, { std::round (cue.duration * 1.0e4) / 1.0e4, cue.fadeTo } };
                        }
                        else
                        {
                            curve.points = eased (cue.fadeFrom, cue.fadeTo, cue.duration);
                            note (Note::Kind::approximated, cue, cue.shape.type == 1 ? "QLab's S-curve, sampled at nine points"
                                                                                    : "QLab's fade shape, sampled at nine points as an S-curve");
                        }

                        item.curves.push_back (std::move (curve));
                        item.attributes.push_back ({ "duration", number (cue.duration, 4) });
                    }
                }
                else if (faded >= 0)
                {
                    note (Note::Kind::approximated, cue, "a #v# in a message that is not a fade: its start value is sent");
                }

                deviceFor (*patch, parts.front(), cue);
                return item;
            }

            //  --- Start, stop and the rest (ZX) ----------------------------------------

            Item control (const Cue& cue)
            {
                const auto target = targetKind (cue.target);

                if (target.empty())
                    return placeholder (cue, "its target is not imported", displayName (cue));

                Item item;
                item.key = "cue:" + cue.id;
                item.name = displayName (cue);
                item.attributes = common (cue);
                item.target = "cue:" + cue.target;

                if (cue.type == "Start")
                {
                    item.kind = "start";
                    return item;
                }

                item.kind = "transport";

                if (cue.type == "Stop")
                {
                    item.attributes.push_back ({ "verb", "hard" });
                }
                else if (cue.type == "Goto")
                {
                    item.attributes.push_back ({ "verb", "jump" });
                }
                else if (cue.type == "Arm" || cue.type == "Disarm")
                {
                    item.attributes.push_back ({ "verb", cue.type == "Arm" ? "enable" : "disable" });
                    note (Note::Kind::approximated, cue, "QLab's " + cue.type + " lasts until it is changed; Go.dot's "
                          "lasts for this run of the show and is never saved");
                }
                else
                {
                    item.attributes.push_back ({ "verb", "advance" });
                    note (Note::Kind::approximated, cue, "a devamp, imported as an advance out of the range that is playing");
                }

                return item;
            }

            //  --- The workspace ---------------------------------------------------------

            void run()
            {
                for (const auto& each : workspace.lists)
                    index (each);

                for (std::size_t at = 0; at < workspace.lists.size(); ++at)
                    if (options.lists.empty() || options.lists.count (static_cast<int> (at)) != 0)
                        for (const auto& child : workspace.lists[at].children)
                            classify (child);

                /*  EVERY STAGE A CANVAS (ALF), in the workspace's order, whether a
                    cue of the lists ticked is on it or not: the rig, as QLab had it. */
                for (const auto& stage : workspace.stages)
                {
                    canvas ("stage:" + stage.id, stage.name.empty() ? std::string ("Stage") : stage.name,
                            stage.width, stage.height, stage.outputs);
                    if (stage.blended)
                        plan.notes.push_back ({ Note::Kind::dropped, -1, {}, "the stage " + stage.name + "'s edge blends "
                                                "are not imported" });
                    if (stage.masked)
                        plan.notes.push_back ({ Note::Kind::dropped, -1, {}, "the stage " + stage.name + "'s mask is not "
                                                "imported" });
                }

                for (std::size_t at = 0; at < workspace.lists.size(); ++at)
                {
                    if (! options.lists.empty() && options.lists.count (static_cast<int> (at)) == 0)
                        continue;

                    list = static_cast<int> (at);
                    const auto& source = workspace.lists[at];

                    if (source.groupMode == 5)
                        note (Note::Kind::approximated, source, "a cart: its grid is not imported, and its cues are a cue "
                              "list fired in order");

                    List out;
                    out.key = "list:" + source.id;
                    out.name = source.name.empty() ? "Cue List" : source.name;
                    out.items = members (source);
                    plan.lists.push_back (std::move (out));
                }

                list = -1;

                std::sort (plan.buses.begin(), plan.buses.end(), [] (const Bus& a, const Bus& b)
                {
                    const auto channel = [] (const Bus& bus) { return bus.channel < 0 ? std::numeric_limits<int>::max() : bus.channel; };
                    return channel (a) != channel (b) ? channel (a) < channel (b) : a.key < b.key;
                });

                if (! plan.buses.empty())
                    plan.notes.push_back ({ Note::Kind::info, -1, {}, "each QLab cue output in use is a mono bus on the "
                                            "interface output of the same number - a QLab patch that routed its cue outputs "
                                            "elsewhere needs the buses patched again" });

                /*  EACH OUTPUT ONCE (ALF): a projector QLab drew several stages to is
                    one output here, showing the stage drawn to it alone; the others
                    are said, to be laid over it as zones. */
                std::map<std::string, std::string> shows;
                for (const auto alone : { true, false })
                    for (const auto& each : plan.canvases)
                        if ((each.outputs.size() == 1) == alone)
                            for (const auto& output : each.outputs)
                                if (shows.count (output) == 0)
                                {
                                    shows[output] = each.key;
                                    plan.outputs.push_back ({ "output:" + output, output, each.key });
                                }

                for (const auto& each : plan.canvases)
                {
                    std::string elsewhere;
                    for (const auto& output : each.outputs)
                        if (shows[output] != each.key)
                            elsewhere += (elsewhere.empty() ? "" : ", ") + output;

                    if (! elsewhere.empty())
                        plan.notes.push_back ({ Note::Kind::info, -1, {}, "the stage " + each.name + " was drawn to " + elsewhere
                                                + ", which show another canvas here: lay it over them as a zone (Show settings, "
                                                  "Video) to see it on those projectors" });
                }

                if (! plan.canvases.empty())
                    plan.notes.push_back ({ Note::Kind::info, -1, {}, "each QLab stage is a canvas of its size, and each output "
                                            "QLab drew to a video output with no display chosen, which draws nothing until a "
                                            "projector is picked for it in Show settings, Video" });
            }
        };
    }

    //==========================================================================
    double decibels (double gain, double floorDb)
    {
        if (! (gain > 0.0))
            return silence;

        const auto db = 20.0 * std::log10 (gain);
        return db <= floorDb ? silence : std::min (db, 12.0);
    }

    std::vector<std::string> splitMessage (const std::string& text)
    {
        std::vector<std::string> out;
        std::string current;
        bool quoted = false, started = false;

        for (std::size_t at = 0; at < text.size(); ++at)
        {
            const auto c = text[at];

            if (quoted)
            {
                current += c;

                if (c == '\\' && at + 1 < text.size())
                    current += text[++at];
                else if (c == '"')
                    quoted = false;

                continue;
            }

            if (c == ' ')
            {
                if (started)
                    out.push_back (current);

                current.clear();
                started = false;
                continue;
            }

            if (c == '"')
                quoted = true;

            current += c;
            started = true;
        }

        if (started)
            out.push_back (current);

        return out;
    }

    std::string atomFor (const std::string& argument)
    {
        if (argument == "\\T") return "T";
        if (argument == "\\F") return "F";
        if (argument == "\\I") return "I";
        if (argument == "\\N") return "N";

        if (isWhole (argument))
        {
            if (const auto value = osc::parseDouble (argument); value.has_value()
                  && *value >= std::numeric_limits<std::int32_t>::min() && *value <= std::numeric_limits<std::int32_t>::max())
                return osc::Value::int32 (static_cast<std::int32_t> (*value)).toAtom();

            return osc::Value::int64 (std::stoll (argument)).toAtom();
        }

        if (isDecimal (argument))
        {
            auto text = argument;

            if (text.back() == '.')
                text += '0';

            if (text.front() == '.' || (text.size() > 1 && text[0] == '-' && text[1] == '.'))
                text.insert (text.front() == '-' ? 1u : 0u, "0");

            if (const auto value = osc::parseDouble (text); value.has_value())
                return osc::Value::float32 (static_cast<float> (*value)).toAtom();
        }

        auto words = argument;

        if (words.size() >= 2 && words.front() == '"' && words.back() == '"')
        {
            words = words.substr (1, words.size() - 2);
            std::string unescaped;

            for (std::size_t at = 0; at < words.size(); ++at)
                unescaped += words[at] == '\\' && at + 1 < words.size() ? words[++at] : words[at];

            words = unescaped;
        }

        return osc::Value::string (words).toAtom();
    }

    Plan walk (const Workspace& workspace, const WalkOptions& options)
    {
        Walker walker { workspace, options, {}, {}, {}, {}, {}, {}, -1, {}, {} };
        walker.run();
        return std::move (walker.plan);
    }
}
