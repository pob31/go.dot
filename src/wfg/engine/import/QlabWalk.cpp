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

                return placeholder (cue, "Go.dot has no " + cue.type + " cue (§46.4)", {});
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

                        if (cue.playlistCrossfade)
                            note (Note::Kind::dropped, cue, "the playlist's crossfade is not imported: its members "
                                  "follow one another (§46.4)");
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

                if (target.empty() || found == byId.end())
                    return placeholder (cue, "its target is not imported", displayName (cue));

                const auto& aimed = *found->second;
                const auto main = std::find_if (cue.fadeLevels.begin(), cue.fadeLevels.end(),
                                                [] (const FadeLevel& f) { return f.row == 0 && f.column == 0; });
                const auto others = cue.fadeLevels.size() - (main == cue.fadeLevels.end() ? 0u : 1u);

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
                        note (Note::Kind::approximated, cue, "a relative fade of " + number (offset, 2) + " dB on a group, "
                              "imported as its trim to " + number (level, 2) + " dB - the offsets summed in show order");
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
        Walker walker { workspace, options, {}, {}, {}, {}, {}, {}, -1 };
        walker.run();
        return std::move (walker.plan);
    }
}
