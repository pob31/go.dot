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

#include <wfg/client/model/NewCueMenus.h>

#include <wfg/client/model/InputList.h>
#include <wfg/client/model/Rack.h>
#include <wfg/client/model/Text.h>
#include <wfg/client/model/Video.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace wfg::client::model
{
    namespace
    {
        /*  "Label — explanation", the dash the settings window's menus and the
            inspector's choices already put between a thing and what it means. */
        std::string lineText (const Choice& choice)
        {
            return choice.explanation.empty() ? choice.label
                                              : choice.label + " — " + choice.explanation;
        }

        std::string lowered (std::string word)
        {
            std::transform (word.begin(), word.end(), word.begin(), [] (unsigned char c)
            {
                return static_cast<char> (c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
            });

            return word;
        }

        /*  THE LINES FOR A RUN OF CHOICES: a heading each time the section
            changes, a rule before every heading but the first, so each part of
            a list reads as a part. */
        void appendChoices (std::vector<MenuLine>& lines, const std::vector<Choice>& choices,
                            bool ruleBeforeFirst)
        {
            std::string section;
            bool first = true;

            for (std::size_t at = 0; at < choices.size(); ++at)
            {
                const auto& choice = choices[at];

                if (first || choice.section != section)
                {
                    if (! choice.section.empty())
                    {
                        if (! first || ruleBeforeFirst)
                            lines.push_back ({ MenuLine::Kind::separator, {}, -1, false });

                        lines.push_back ({ MenuLine::Kind::header, choice.section, -1, false });
                    }

                    section = choice.section;
                    first = false;
                }

                lines.push_back ({ MenuLine::Kind::item, lineText (choice), static_cast<int> (at), false });
            }
        }

        /*  The list a cue's parent chain ends in: the first ancestor with no
            kind, which is a list - a header, footer or persistent member names
            its group or list as its parent, so the chain never stops on a
            section. Bounded, so a picture caught half-way through an edit
            cannot walk for ever. */
        std::string listOf (const tree::TreeSnapshot& snapshot, const std::string& cueId)
        {
            auto at = text (snapshot, "/godot/cue/" + cueId + "/parent");

            for (int depth = 0; depth < 64 && ! at.empty(); ++depth)
            {
                if (text (snapshot, "/godot/cue/" + at + "/kind").empty())
                    return at;

                at = text (snapshot, "/godot/cue/" + at + "/parent");
            }

            return {};
        }

        bool hasPickedAncestor (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                                const std::vector<std::string>& picked)
        {
            auto at = text (snapshot, "/godot/cue/" + cueId + "/parent");

            for (int depth = 0; depth < 64 && ! at.empty(); ++depth)
            {
                if (std::find (picked.begin(), picked.end(), at) != picked.end())
                    return true;

                at = text (snapshot, "/godot/cue/" + at + "/parent");
            }

            return false;
        }
    }

    bool opensList (const std::string& kind)
    {
        /*  AND MEDIA, since namespace draft §38: files as ever, or files born
            from a cue template. */
        return kind == "group" || kind == "transport" || kind == "midi" || kind == "mic"
            || kind == "video" || kind == "media";
    }

    //==============================================================================
    const std::vector<Choice>& groupChoices()
    {
        /*  FIVE, the author's number (2026-09-27): the automatic sequence and
            the one on GO behave too differently in a show to share a line, and
            a shuffle is an automatic sequence in a fresh order - automatic by
            force, since a manual group ignores `selection`. */
        static const std::vector<Choice> choices {
            { "Timeline group", "members start together, each after its own pre-wait", "group",
              { { "mode", "timeline" } }, {}, false, false, {} },
            { "Sequential group, on GO", "one member per GO", "group",
              { { "mode", "sequence" }, { "advance", "manual" } }, {}, false, false, {} },
            { "Sequential group, automatic", "each member when the one before ends", "group",
              { { "mode", "sequence" }, { "advance", "auto" } }, {}, false, false, {} },
            { "Shuffle group", "automatic, in a fresh order each round", "group",
              { { "mode", "sequence" }, { "advance", "auto" }, { "selection", "shuffle" } }, {}, false, false, {} },
            { "Sampler group", "faders and pads start its members", "group",
              { { "mode", "sampler" } }, {}, true, false, {} },
        };

        return choices;
    }

    const std::vector<Choice>& transportChoices()
    {
        static const std::vector<Choice> choices {
            { "Stop", "at once", "transport", { { "verb", "hard" } }, "Stop", false, true, {} },
            { "Stop after this member", "a group finishes the member playing", "transport",
              { { "verb", "afterMember" } }, "Stop", false, true, {} },
            { "Stop after this round", "a group finishes the round", "transport",
              { { "verb", "afterIteration" } }, "Stop", false, true, {} },
            { "Advance", "a ranged cue leaves the slice it is on", "transport",
              { { "verb", "advance" } }, "Stop", false, true, {} },

            /*  START IS ITS OWN KIND (the author, 2026-09-27: the list, not a
                merge), listed here because it is what the others are - a cue
                that does something to another cue. */
            { "Start", "fires the target, wherever it sits", "start", {}, "Start", false, true, {} },

            { "Rec", "the take's Rec button", "transport", { { "verb", "record" } }, "On a take", false, true, {} },
            { "Loop", "the take's Loop button", "transport", { { "verb", "loop" } }, "On a take", false, true, {} },
            { "Overdub", "a layer begun or closed", "transport", { { "verb", "overdub" } }, "On a take", false, true, {} },
            { "Clear", "the take emptied", "transport", { { "verb", "clear" } }, "On a take", false, true, {} },

            /*  THE SWITCHES FOR THIS RUN AND THE SHOW'S OWN PARK (the author,
                2026-10-05, namespace draft §27). Not arm and disarm: arm already
                means a voice made ready. "and Go" is a second line rather than a
                question after the first, because a line makes one cue as it
                stands; the inspector's switch turns one into the other (PV). */
            { "Enable", "the target runs again, until the show closes", "transport",
              { { "verb", "enable" } }, "For this run", false, true, {} },
            { "Disable", "the target is skipped, until the show closes", "transport",
              { { "verb", "disable" } }, "For this run", false, true, {} },
            { "Jump to", "standby moves to the target", "transport",
              { { "verb", "jump" } }, "Standby", false, true, {} },
            { "Jump to and Go", "standby moves to the target and fires it", "transport",
              { { "verb", "jump" }, { "andGo", "true" } }, "Standby", false, true, {} },
        };

        return choices;
    }

    const std::vector<Choice>& midiChoices()
    {
        /*  In the order a theatre sends them: a desk's scene and a fader
            first, notes after, SysEx last. */
        static const std::vector<Choice> choices {
            { "Program change", "recall a scene or a patch", "midi",
              { { "type", "programChange" } }, "Scenes and controls", false, false, {} },
            { "Control change", "move a fader or a switch", "midi",
              { { "type", "controlChange" } }, "Scenes and controls", false, false, {} },
            { "Note on", {}, "midi", { { "type", "noteOn" } }, "Notes", false, false, {} },
            { "Note off", {}, "midi", { { "type", "noteOff" } }, "Notes", false, false, {} },
            { "Aftertouch", "pressure on one note", "midi", { { "type", "aftertouch" } }, "Notes", false, false, {} },
            { "Pitch bend", {}, "midi", { { "type", "pitchBend" } }, "Whole channel", false, false, {} },
            { "Channel pressure", "pressure on the whole channel", "midi",
              { { "type", "channelPressure" } }, "Whole channel", false, false, {} },
            { "SysEx", "bytes copied from a manual", "midi", { { "type", "sysex" } }, "System", false, false, {} },
        };

        return choices;
    }

    std::vector<Choice> micChoices (const tree::TreeSnapshot& snapshot)
    {
        std::vector<Choice> choices;
        const auto rack = readRack (snapshot);

        for (const auto& input : readInputs (snapshot))
        {
            const auto section = input.name + " — " + lowered (input.widthWord()) + " input";

            for (const auto& channel : rack.channels)
            {
                /*  WHAT THE ENGINE WOULD ACCEPT (rackChannel,class): one input
                    channel into mono or mono-to-stereo, two into stereo; wider
                    comes later. A shared channel is a bus with a chain, never
                    claimed by a cue. */
                const auto fits = input.width == 1 ? (channel.channelClass == "mono"
                                                        || channel.channelClass == "monoToStereo")
                                                   : (input.width == 2 && channel.channelClass == "stereo");

                if (! fits || text (snapshot, "/godot/slot/" + channel.id + "/access") == "shared")
                    continue;

                choices.push_back ({ "through " + channel.name, lowered (channel.classWord()), "mic",
                                     { { "input", input.id }, { "channel", channel.id } },
                                     section, false, false, {} });
            }
        }

        choices.push_back ({ "No input yet", "set it in the inspector", "mic", {}, {}, false, false, {} });
        return choices;
    }

    std::vector<Choice> videoChoices (const tree::TreeSnapshot& snapshot)
    {
        /*  WHAT IT SHOWS, ON WHICH CANVAS (Phase 8a, the author's words: a fill
            is a background, a mask an overlay). One part per canvas, born on
            it, and a last line on none when the show has several - or only
            the lines on none when it has no canvas yet. What is offered grows
            with what is built: a mask and a picture join when they draw. */
        std::vector<Choice> choices;

        for (const auto& canvas : readCanvases (snapshot))
        {
            choices.push_back ({ "Picture", "a picture file, chosen in the inspector", "video",
                                 { { "source", "picture" }, { "canvas", canvas.id } },
                                 "On " + canvas.label(), false, false, {} });
            choices.push_back ({ "Movie", "a HAP movie, chosen in the inspector", "video",
                                 { { "source", "movie" }, { "canvas", canvas.id } },
                                 "On " + canvas.label(), false, false, {} });
            choices.push_back ({ "Fill", "one colour over the whole canvas, behind", "video",
                                 { { "source", "fill" }, { "canvas", canvas.id } },
                                 "On " + canvas.label(), false, false, {} });
            choices.push_back ({ "Mask", "a shape laid over, in black", "video",
                                 { { "source", "mask" }, { "canvas", canvas.id }, { "layer", "100" },
                                   { "shape", "0.25 0.25 0.75 0.25 0.75 0.75 0.25 0.75" } },
                                 "On " + canvas.label(), false, false, {} });
        }

        choices.push_back ({ "Fill, on no canvas yet", "set it in the inspector", "video",
                             { { "source", "fill" } }, {}, false, false, {} });

        /*  AND EACH PICTURE TEMPLATE (namespace draft §38): born on the canvas
            the template says, its file chosen in the inspector as any
            picture's is. */
        for (const auto& row : readCueTemplates (snapshot))
        {
            if (row.kind == "media")
                continue;

            Choice line { row.label(), "a " + row.kindWord() + " from a template", "video", {},
                          "From a template", false, false, row.id };
            choices.push_back (std::move (line));
        }

        return choices;
    }

    //==============================================================================
    std::string CueTemplateRow::label() const
    {
        return name.empty() ? std::string ("Template") : name;
    }

    std::string CueTemplateRow::kindWord() const
    {
        return kind == "media" ? std::string ("audio cue") : kind.empty() ? std::string ("cue") : kind;
    }

    std::string CueTemplateRow::carriesWords() const
    {
        /*  READ OFF THE FRAGMENT BY WHAT IT HOLDS, a word per part somebody
            would recognise; the rows themselves are the engine's to read. */
        const auto has = [this] (const char* attribute)
        {
            return parts.find (std::string (" ") + attribute + "=\"") != std::string::npos;
        };

        const auto count = [this] (const char* element)
        {
            std::size_t found = 0;

            for (auto at = parts.find (element); at != std::string::npos; at = parts.find (element, at + 1))
                ++found;

            return found;
        };

        std::vector<std::string> said;

        if (has ("level"))                            said.push_back ("level");
        if (has ("dca"))                              said.push_back ("DCA");
        if (has ("directOut") || has ("sharedOut"))   said.push_back ("routing");
        if (has ("canvas"))                           said.push_back ("canvas");

        if (has ("opacity") || has ("scale") || has ("rotation") || has ("offsetX") || has ("offsetY"))
            said.push_back ("geometry");

        if (has ("contrast") || has ("saturation") || has ("gamma") || has ("hue"))
            said.push_back ("grade");

        if (parts.find (" eq") != std::string::npos && kind == "media")
            said.push_back ("EQ");

        if (const auto sends = count ("<Send "); sends > 0)
            said.push_back (std::to_string (sends) + (sends == 1 ? " send" : " sends"));

        if (const auto effects = count ("<Fx "); effects > 0)
            said.push_back (std::to_string (effects) + (effects == 1 ? " effect" : " effects"));

        if (has ("rate"))                             said.push_back ("speed");

        if (said.empty())
            return "the defaults";

        std::string out;

        for (const auto& word : said)
            out += (out.empty() ? "" : ", ") + word;

        return out;
    }

    std::vector<CueTemplateRow> readCueTemplates (const tree::TreeSnapshot& snapshot)
    {
        std::vector<CueTemplateRow> rows;

        for (const auto& id : words (text (snapshot, "/godot/cueTemplate/order")))
        {
            CueTemplateRow row;
            row.id = id;
            row.name = text (snapshot, "/godot/cueTemplate/" + id + "/name");
            row.kind = text (snapshot, "/godot/cueTemplate/" + id + "/kind");
            row.parts = text (snapshot, "/godot/cueTemplate/" + id + "/parts");
            rows.push_back (std::move (row));
        }

        return rows;
    }

    std::vector<CueTemplateRow> templatesFor (const std::vector<CueTemplateRow>& all, const std::string& cueKind,
                                              const std::string& source)
    {
        std::vector<CueTemplateRow> out;

        for (const auto& row : all)
            if ((cueKind == "media" && row.kind == "media") || (cueKind == "video" && row.kind == source))
                out.push_back (row);

        return out;
    }

    std::vector<Choice> mediaChoices (const tree::TreeSnapshot& snapshot)
    {
        std::vector<Choice> choices;
        choices.push_back ({ "Files...", "chosen from the disk, a cue each", "media", {}, {}, false, false, {} });

        for (const auto& row : readCueTemplates (snapshot))
        {
            if (row.kind != "media")
                continue;

            Choice line { row.label(), "files chosen next, each cue with " + row.carriesWords(), "media", {},
                          "From a template", false, false, row.id };
            choices.push_back (std::move (line));
        }

        return choices;
    }

    std::vector<MenuLine> mediaMenu (const std::vector<Choice>& choices, const std::string& destination)
    {
        std::vector<MenuLine> lines;
        lines.push_back ({ MenuLine::Kind::note, "New audio cues, " + destination, -1, false });
        appendChoices (lines, choices, true);

        if (choices.size() <= 1)
            lines.push_back ({ MenuLine::Kind::note,
                               "No template yet: pick a cue, then Edit, Save as template.", -1, false });

        return lines;
    }

    //==============================================================================
    Wrap wrapOf (const tree::TreeSnapshot& snapshot, const std::vector<std::string>& picked)
    {
        Wrap out;
        out.cues = picked;

        if (picked.empty())
            return out;

        std::string list;
        bool allMedia = true;

        for (const auto& id : picked)
        {
            const auto kind = text (snapshot, "/godot/cue/" + id + "/kind");

            if (kind.empty())
            {
                out.why = "Something picked is not a cue.";
                return out;
            }

            const auto itsList = listOf (snapshot, id);

            if (list.empty())
                list = itsList;
            else if (itsList != list)
            {
                out.why = "The picked cues are in two lists: pick cues from one list to put them in a group.";
                return out;
            }

            /*  A picked group carries its picked members in with it, as the
                engine does, so they are not counted twice - and a sampler asks
                about the cues it would hold, not about the ones inside them. */
            if (hasPickedAncestor (snapshot, id, picked))
                continue;

            ++out.count;
            allMedia = allMedia && kind == "media";
        }

        out.allMedia = out.count > 0 && allMedia;
        return out;
    }

    std::vector<MenuLine> groupMenu (const Wrap& wrap, const std::string& destination)
    {
        std::vector<MenuLine> lines;
        const auto& choices = groupChoices();

        if (wrap.possible())
        {
            lines.push_back ({ MenuLine::Kind::header,
                               wrap.count == 1 ? std::string ("Put the picked cue in a new…")
                                               : "Put the " + std::to_string (wrap.count) + " picked cues in a new…",
                               -1, false });

            for (std::size_t at = 0; at < choices.size(); ++at)
                if (! choices[at].mediaOnly || wrap.allMedia)
                    lines.push_back ({ MenuLine::Kind::item, lineText (choices[at]), static_cast<int> (at), true });

            lines.push_back ({ MenuLine::Kind::separator, {}, -1, false });
        }
        else if (! wrap.why.empty())
        {
            lines.push_back ({ MenuLine::Kind::note, wrap.why, -1, false });
            lines.push_back ({ MenuLine::Kind::separator, {}, -1, false });
        }

        lines.push_back ({ MenuLine::Kind::header, "An empty new group, " + destination, -1, false });

        for (std::size_t at = 0; at < choices.size(); ++at)
            lines.push_back ({ MenuLine::Kind::item, lineText (choices[at]), static_cast<int> (at), false });

        return lines;
    }

    std::vector<MenuLine> transportMenu (const std::string& aimName, const std::string& destination)
    {
        std::vector<MenuLine> lines;

        lines.push_back ({ MenuLine::Kind::note,
                           aimName.empty() ? "No target yet: it lands " + destination
                                           : "Aimed at " + aimName + ", and placed after it",
                           -1, false });

        appendChoices (lines, transportChoices(), true);
        return lines;
    }

    std::vector<MenuLine> midiMenu (const std::string& destination)
    {
        std::vector<MenuLine> lines;
        lines.push_back ({ MenuLine::Kind::note, "A new MIDI cue, " + destination, -1, false });
        appendChoices (lines, midiChoices(), true);
        return lines;
    }

    std::vector<MenuLine> micMenu (const tree::TreeSnapshot& snapshot, const std::vector<Choice>& choices,
                                   const std::string& destination)
    {
        std::vector<MenuLine> lines;
        lines.push_back ({ MenuLine::Kind::note, "A new mic cue, " + destination, -1, false });

        /*  NEVER ONLY THE LAST LINE WITH NO WORD WHY: an empty rack or no
            named input is said, with where to make them. */
        if (readInputs (snapshot).empty())
            lines.push_back ({ MenuLine::Kind::note,
                               "No named inputs yet: name them in Show settings, on the Inputs tab.", -1, false });
        else if (readRack (snapshot).channels.empty())
            lines.push_back ({ MenuLine::Kind::note,
                               "No rack channels yet: add one in Show settings, on the Rack tab.", -1, false });

        std::vector<Choice> placed;
        std::vector<int> indices;

        for (std::size_t at = 0; at < choices.size(); ++at)
            if (! choices[at].section.empty())
            {
                placed.push_back (choices[at]);
                indices.push_back (static_cast<int> (at));
            }

        const auto before = lines.size();
        appendChoices (lines, placed, true);

        for (auto at = before; at < lines.size(); ++at)
            if (lines[at].kind == MenuLine::Kind::item)
                lines[at].choice = indices[static_cast<std::size_t> (lines[at].choice)];

        lines.push_back ({ MenuLine::Kind::separator, {}, -1, false });

        for (std::size_t at = 0; at < choices.size(); ++at)
            if (choices[at].section.empty())
                lines.push_back ({ MenuLine::Kind::item, lineText (choices[at]), static_cast<int> (at), false });

        return lines;
    }

    std::vector<MenuLine> videoMenu (const tree::TreeSnapshot& snapshot, const std::vector<Choice>& choices,
                                     const std::string& destination)
    {
        std::vector<MenuLine> lines;
        lines.push_back ({ MenuLine::Kind::note, "A new video cue, " + destination, -1, false });

        /*  NEVER ONLY THE LAST LINE WITH NO WORD WHY, as for a mic cue. */
        if (readCanvases (snapshot).empty())
            lines.push_back ({ MenuLine::Kind::note,
                               "No canvas yet: make one in Show settings, on the Video tab.", -1, false });

        std::vector<Choice> placed;
        std::vector<int> indices;

        for (std::size_t at = 0; at < choices.size(); ++at)
            if (! choices[at].section.empty())
            {
                placed.push_back (choices[at]);
                indices.push_back (static_cast<int> (at));
            }

        const auto before = lines.size();
        appendChoices (lines, placed, true);

        for (auto at = before; at < lines.size(); ++at)
            if (lines[at].kind == MenuLine::Kind::item)
                lines[at].choice = indices[static_cast<std::size_t> (lines[at].choice)];

        lines.push_back ({ MenuLine::Kind::separator, {}, -1, false });

        for (std::size_t at = 0; at < choices.size(); ++at)
            if (choices[at].section.empty())
                lines.push_back ({ MenuLine::Kind::item, lineText (choices[at]), static_cast<int> (at), false });

        return lines;
    }

    //==============================================================================
    std::string verbWord (const std::string& verb, bool andGo)
    {
        if (verb == "hard" || verb.empty())  return "stop";
        if (verb == "fade")                  return "fade out";
        if (verb == "afterMember")           return "member";
        if (verb == "afterIteration")        return "round";
        if (verb == "record")                return "rec";
        if (verb == "jump")                  return andGo ? "jump+go" : "jump";

        return verb;    // advance, loop, overdub, clear, enable, disable: already a word, already short
    }
}
