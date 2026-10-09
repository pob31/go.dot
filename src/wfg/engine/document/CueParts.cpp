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

#include <wfg/engine/document/CueParts.h>

#include <wfg/engine/command/Command.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/Sequence.h>
#include <wfg/engine/document/ShowDocument.h>

#include <algorithm>

namespace wfg::doc::parts
{
    namespace
    {
        struct Entry
        {
            Part part;
            std::string_view word;
            std::vector<std::string_view> elements;     ///< the kinds of cue it fits
            std::vector<std::string_view> rows;         ///< on the cue itself
            std::string_view child;                     ///< "Send", "Fx", "Range", or empty
            std::vector<std::string_view> childRows;    ///< of each such child
        };

        const std::vector<Entry>& table()
        {
            static const std::vector<Entry> entries {
                { Part::eq, "eq", { "Media", "Mic" },
                  { "eqOn", "eqHpf", "eqHpfFreq", "eqLpf", "eqLpfFreq",
                    "eqB1On", "eqB1Shape", "eqB1Freq", "eqB1Gain", "eqB1Q",
                    "eqB2On", "eqB2Freq", "eqB2Gain", "eqB2Q",
                    "eqB3On", "eqB3Freq", "eqB3Gain", "eqB3Q",
                    "eqB4On", "eqB4Shape", "eqB4Freq", "eqB4Gain", "eqB4Q" },
                  {}, {} },

                /*  A send is the level of a cue at a mix, matched by the mix it
                    feeds: its lane stays behind (WV). */
                { Part::sends, "sends", { "Media", "Mic" }, {}, "Send", { "bus", "level", "on" } },

                /*  The chain, in order, each insert with its state: the file
                    named by `stateFile` is immutable in the bundle, so two cues
                    naming it share nothing that either can change. */
                { Part::fx, "fx", { "Media", "Mic" }, {}, "Fx", { "plugin", "enabled", "values", "stateFile" } },

                { Part::time, "time", { "Media", "Video" }, { "startOffset", "rate", "rateMode" },
                  "Range", { "name", "in", "out", "loops" } },

                { Part::speed, "speed", { "Media", "Video" }, { "rate", "rateMode" }, {}, {} },

                //  The DCA's mapping goes where its mark goes (namespace draft §50, ACB).
                { Part::mix, "mix", { "Media", "Mic", "Video" },
                  { "level", "dca", "dcaCurve", "dcaOffset", "colour", "directOut", "sharedOut" }, {}, {} },

                //  How a strip plays it, a sound's or a picture's (namespace draft §49).
                { Part::play, "play", { "Media", "Video" },
                  { "release", "secondPress", "velocity", "velocityFloor", "pressure",
                    "releaseFade", "initialLevel" }, {}, {} },

                /*  Everything about how a picture lies on its canvas, but what
                    it is: `source` and `file` stay the cue's own. */
                { Part::picture, "picture", { "Video" },
                  { "canvas", "layer", "blend", "opacity", "paint", "fadeIn", "fit", "scale",
                    "offsetX", "offsetY", "rotation", "flipH", "flipV",
                    "contrast", "saturation", "gamma", "hue",
                    "curveLuma", "curveRed", "curveGreen", "curveBlue",
                    "shape", "feather", "invert" }, {}, {} },
            };

            return entries;
        }

        const Entry& entryFor (Part part)
        {
            const auto& entries = table();
            return *std::find_if (entries.begin(), entries.end(),
                                  [part] (const Entry& e) { return e.part == part; });
        }
    }

    std::string_view wordFor (Part part) noexcept
    {
        for (const auto& entry : table())
            if (entry.part == part)
                return entry.word;

        return {};
    }

    std::optional<Part> partForWord (std::string_view word) noexcept
    {
        for (const auto& entry : table())
            if (entry.word == word)
                return entry.part;

        return std::nullopt;
    }

    std::optional<std::vector<Part>> partsForWords (std::string_view words)
    {
        std::vector<Part> out;
        std::size_t at = 0;

        while (at < words.size())
        {
            while (at < words.size() && words[at] == ' ')
                ++at;

            const auto end = std::min (words.find (' ', at), words.size());

            if (end > at)
            {
                const auto part = partForWord (words.substr (at, end - at));

                if (! part.has_value() || std::find (out.begin(), out.end(), *part) != out.end())
                    return std::nullopt;

                out.push_back (*part);
            }

            at = end;
        }

        if (out.empty())
            return std::nullopt;

        return out;
    }

    std::string wordsFor (const std::vector<Part>& chosen)
    {
        std::string out;

        for (const auto part : chosen)
            out += (out.empty() ? "" : " ") + std::string (wordFor (part));

        return out;
    }

    bool fits (Part part, std::string_view element) noexcept
    {
        const auto& elements = entryFor (part).elements;
        return std::find (elements.begin(), elements.end(), element) != elements.end();
    }

    const std::vector<std::string_view>& rowsOf (Part part)          { return entryFor (part).rows; }
    std::string_view childOf (Part part) noexcept                    { return entryFor (part).child; }
    const std::vector<std::string_view>& childRowsOf (Part part)     { return entryFor (part).childRows; }

    std::vector<Part> templatePartsFor (std::string_view element)
    {
        if (element == "Media")
            return { Part::mix, Part::play, Part::eq, Part::sends, Part::fx, Part::speed };

        if (element == "Video")
            return { Part::mix, Part::play, Part::picture, Part::speed };

        return {};
    }
}

namespace wfg::doc
{
    namespace
    {
        const juce::Identifier idOf { "id" };

        std::string idText (const juce::ValueTree& node)
        {
            return node[idOf].toString().toStdString();
        }

        std::vector<juce::ValueTree> childrenOfType (const juce::ValueTree& node, std::string_view type)
        {
            std::vector<juce::ValueTree> out;
            const juce::String name { std::string (type) };

            for (const auto& child : node)
                if (child.getType().toString() == name)
                    out.push_back (child);

            return out;
        }

        /*  A ROW OF THE FRAGMENT AS TEXT, or the row's default when the
            fragment does not carry it - which is what absent means, since the
            writer omits a default (WO: an absent row is a value). */
        std::string textOf (const Attribute& attribute, const juce::ValueTree& node)
        {
            if (const auto text = CanonicalXml::attributeText (attribute, node))
                return *text;

            return std::string (attribute.defaultText());
        }
    }

    //==============================================================================
    std::string ShowDocument::partFragmentOf (const std::string& partWords, const std::string& cueId) const
    {
        const auto named = parts::partsForWords (partWords);
        const auto cue = findById (cueId);

        if (! named.has_value() || ! cue.isValid())
            return {};

        const auto element = cue.getType().toString().toStdString();

        for (const auto part : *named)
            if (! parts::fits (part, element))
                return {};

        /*  A STRIPPED COPY: the cue's element and identity, and of its rows and
            children only what the parts hold. The identity is there because
            the reader insists on one, and it is read into a registry of its
            own, so it reserves nothing in the show it is pasted into. */
        juce::ValueTree copy { cue.getType() };
        copy.setProperty (idOf, cue[idOf], nullptr);

        for (const auto part : *named)
        {
            for (const auto row : parts::rowsOf (part))
            {
                const juce::Identifier property { juce::String (std::string (row)) };

                if (cue.hasProperty (property))
                    copy.setProperty (property, cue[property], nullptr);
            }

            const auto child = parts::childOf (part);

            if (child.empty())
                continue;

            for (const auto& original : childrenOfType (cue, child))
            {
                juce::ValueTree made { original.getType() };
                made.setProperty (idOf, original[idOf], nullptr);

                for (const auto row : parts::childRowsOf (part))
                {
                    const juce::Identifier property { juce::String (std::string (row)) };

                    if (original.hasProperty (property))
                        made.setProperty (property, original[property], nullptr);
                }

                copy.appendChild (made, nullptr);
            }
        }

        return CanonicalXml::writePartFragment (parts::wordsFor (*named), copy);
    }

    EditResult ShowDocument::copyPartToClipboard (const std::string& partWords, const std::string& cueId)
    {
        if (! parts::partsForWords (partWords).has_value())
            return EditResult::failed (reason::badValue);

        if (! findById (cueId).isValid())
            return EditResult::failed (reason::unknownId);

        auto fragment = partFragmentOf (partWords, cueId);

        if (fragment.empty())
            return EditResult::failed (reason::typeMismatch);

        partClipboard = std::move (fragment);
        return EditResult::succeeded (cueId);
    }

    //==============================================================================
    EditResult ShowDocument::pastePart (const std::string& fragment, const std::vector<std::string>& cueIds,
                                        const std::vector<std::string>& ids)
    {
        if (auto refusal = refuseIfLocked())
            return *refusal;

        const auto read = CanonicalXml::readPartFragment (fragment);

        if (! read.ok)
            return EditResult::failed (reason::badValue);

        const auto named = parts::partsForWords (read.parts);

        if (! named.has_value() || cueIds.empty())
            return EditResult::failed (reason::badValue);

        const auto& source = read.node;
        const auto sourceElement = source.getType().toString().toStdString();
        const auto& schema = Schema::instance();

        const auto has = [&named] (parts::Part part)
        {
            return std::find (named->begin(), named->end(), part) != named->end();
        };

        //------------------------------------------------------------------
        /*  EVERY REFUSAL IS ASKED BEFORE THE FIRST WRITE, so a paste that is
            refused has changed nothing. What is written after is what the
            doors below have already been asked about: rows the schema read
            out of the fragment, sends to buses that are there, inserts of
            entries that are there. */
        struct Target
        {
            juce::ValueTree cue;
            std::string element;

            /*  A sound locked to a movie that is pasted onto too: what it
                shares with the movie - its start offset, its speed, its
                Ranges - comes from the movie (namespace draft 37.5), so it
                takes the rest of the paste and leaves those to the movie. */
            bool sharedFromMovie = false;
        };

        std::vector<Target> targets;

        for (const auto& id : cueIds)
        {
            if (std::any_of (targets.begin(), targets.end(),
                             [&id] (const Target& t) { return idText (t.cue) == id; }))
                continue;

            auto cue = findById (id);

            if (! cue.isValid())
                return EditResult::failed (reason::unknownId);

            Target target { cue, cue.getType().toString().toStdString() };

            for (const auto part : *named)
            {
                if (! parts::fits (part, target.element))
                    return EditResult::failed (reason::typeMismatch);

                /*  AN EFFECTS CHAIN ONLY BETWEEN CUES OF ONE KIND (WU): a media
                    cue's inserts are entries of the show's set, a mic cue's are
                    plugins of the rack channel it plays through. */
                if (part == parts::Part::fx && target.element != sourceElement)
                    return EditResult::failed (reason::typeMismatch);
            }

            /*  A MIC'S CHAIN ONLY ONTO A MIC OF THE SAME CHANNEL: the plugins
                are that channel's, and a chain taken off and nothing put back
                would be a paste that destroyed something. */
            if (has (parts::Part::fx) && target.element == "Mic")
            {
                for (const auto& fx : childrenOfType (source, "Fx"))
                {
                    const auto entry = findById (fx["plugin"].toString().toStdString());

                    if (entry.isValid() && entry.getParent()[idOf].toString() != cue["channel"].toString())
                        return EditResult::failed (reason::badValue);
                }
            }

            if ((has (parts::Part::time) || has (parts::Part::speed)) && followsAMovie (cue))
            {
                const auto movie = cue["lockedTo"].toString().toStdString();

                if (std::find (cueIds.begin(), cueIds.end(), movie) == cueIds.end())
                    return EditResult::failed (reason::lockedToMovie);

                target.sharedFromMovie = true;
            }

            targets.push_back (std::move (target));
        }

        //------------------------------------------------------------------
        std::vector<std::string> drawn;
        std::size_t supplied = 0;

        const auto nextId = [&ids, &supplied]
        {
            return supplied < ids.size() ? ids[supplied++] : std::string {};
        };

        const auto addressOf = [] (const juce::ValueTree& node, std::string_view row)
        {
            return "/godot/" + std::string (addressOwnerFor (node.getType().toString().toStdString()))
                     + "/" + idText (node) + "/" + std::string (row);
        };

        /*  A ROW FROM THE SOURCE ONTO THE TARGET, unless the target already
            says it: a write that changes nothing has no place in the step. A
            row the target's kind does not carry, or the source's kind did not
            (a movie has no `rateMode`), is left as it is. */
        const auto writeRow = [this, &schema, &addressOf] (const juce::ValueTree& from, const juce::ValueTree& to,
                                                           std::string_view row,
                                                           std::optional<std::string> forced = std::nullopt)
                              -> EditResult
        {
            const auto* toAttribute = schema.attribute (to.getType().toString().toStdString(), row);
            const auto* fromAttribute = schema.attribute (from.getType().toString().toStdString(), row);

            if (toAttribute == nullptr || fromAttribute == nullptr)
                return EditResult::succeeded();

            const auto text = forced.has_value() ? *forced : textOf (*fromAttribute, from);
            const auto address = addressOf (to, row);

            if (getAttribute (address) == text)
                return EditResult::succeeded();

            return setAttribute (address, text);
        };

        for (auto& target : targets)
        {
            const auto cueId = idText (target.cue);

            for (const auto part : *named)
            {
                const auto shared = part == parts::Part::time || part == parts::Part::speed;

                if (shared && target.sharedFromMovie)
                    continue;

                const auto sourceRanges = childrenOfType (source, "Range");

                for (const auto row : parts::rowsOf (part))
                {
                    /*  A MOVIE PLAYS A START OFFSET OR A PLAYLIST, NEVER BOTH
                        (namespace draft 37.5): Ranges coming in put its offset
                        back to nought. */
                    std::optional<std::string> forced;

                    if (part == parts::Part::time && row == "startOffset"
                          && target.element == "Video" && ! sourceRanges.empty())
                        forced = "0";

                    if (const auto written = writeRow (source, target.cue, row, forced); ! written.ok)
                        return written;
                }

                //  SENDS, matched by the bus they feed.
                if (part == parts::Part::sends)
                {
                    const auto wanted = childrenOfType (source, "Send");

                    for (const auto& send : childrenOfType (target.cue, "Send"))
                    {
                        const auto bus = send["bus"].toString();
                        const auto kept = std::any_of (wanted.begin(), wanted.end(),
                                                       [&bus] (const juce::ValueTree& w) { return w["bus"].toString() == bus; });

                        if (! kept)
                            if (const auto removed = remove (idText (send)); ! removed.ok)
                                return removed;
                    }

                    for (const auto& want : wanted)
                    {
                        const auto busId = want["bus"].toString().toStdString();

                        /*  A MIX THE SHOW DOES NOT HAVE - a fragment from another
                            show, or a bus taken away since the copy - is passed
                            over: there is nowhere to send to. */
                        if (! findById (busId).hasType ("Bus"))
                            continue;

                        juce::ValueTree send;

                        for (const auto& existing : childrenOfType (target.cue, "Send"))
                            if (existing["bus"].toString().toStdString() == busId)
                                send = existing;

                        if (! send.isValid())
                        {
                            const auto made = createSend (cueId, busId, nextId());

                            if (! made.ok)
                                return made;

                            drawn.push_back (made.id);
                            send = findById (made.id);
                        }

                        for (const auto row : { "level", "on" })
                            if (const auto written = writeRow (want, send, row); ! written.ok)
                                return written;
                    }
                }

                /*  THE CHAIN, swapped whole and rebuilt in the source's order:
                    an insert's place in the chain is part of what it does. */
                if (part == parts::Part::fx)
                {
                    for (const auto& fx : childrenOfType (target.cue, "Fx"))
                        if (const auto removed = remove (idText (fx)); ! removed.ok)
                            return removed;

                    for (const auto& want : childrenOfType (source, "Fx"))
                    {
                        const auto pluginId = want["plugin"].toString().toStdString();

                        //  An entry the show no longer declares: nothing to switch in.
                        if (! findById (pluginId).hasType ("Plugin"))
                            continue;

                        const auto made = createFx (cueId, pluginId, nextId());

                        if (! made.ok)
                            return made;

                        drawn.push_back (made.id);
                        const auto fx = findById (made.id);

                        for (const auto row : { "enabled", "values", "stateFile" })
                            if (const auto written = writeRow (want, fx, row); ! written.ok)
                                return written;
                    }
                }

                //  THE PLAYLIST, replaced whole: document order is playing order.
                if (part == parts::Part::time)
                {
                    for (const auto& range : childrenOfType (target.cue, "Range"))
                        if (const auto removed = remove (idText (range)); ! removed.ok)
                            return removed;

                    for (const auto& want : sourceRanges)
                    {
                        const auto made = createRange (cueId,
                                                       static_cast<double> (want.getProperty ("in", 0.0)),
                                                       static_cast<double> (want.getProperty ("out", 0.0)),
                                                       nextId());

                        if (! made.ok)
                            return made;

                        drawn.push_back (made.id);
                        const auto range = findById (made.id);

                        for (const auto row : { "name", "loops" })
                            if (const auto written = writeRow (want, range, row); ! written.ok)
                                return written;
                    }
                }
            }

            /*  AND A MOVIE'S SOUNDS MADE ITS AGAIN, once, in this step: a Range
                taken away is not one of the edits that carries them itself. */
            if (target.element == "Video" && (has (parts::Part::time) || has (parts::Part::speed)))
                keepSoundsWith (target.cue);
        }

        std::string joined;

        for (const auto& id : drawn)
            joined += (joined.empty() ? "" : " ") + id;

        return EditResult::succeeded (joined);
    }

    //==============================================================================
    namespace
    {
        /*  THE KIND A TEMPLATE OF THIS CUE IS: "media", or a video cue's source
            - what the Add menu offers it beside. Empty for a cue no template is
            made of. */
        std::string templateKindOf (const juce::ValueTree& cue)
        {
            if (cue.hasType ("Media"))
                return "media";

            if (cue.hasType ("Video"))
            {
                const auto source = cue.getProperty ("source").toString().toStdString();
                return source.empty() ? std::string ("fill") : source;
            }

            return {};
        }

        std::string templateWordsFor (const juce::ValueTree& cue)
        {
            return parts::wordsFor (parts::templatePartsFor (cue.getType().toString().toStdString()));
        }
    }

    EditResult ShowDocument::createCueTemplate (const std::string& name, const std::string& cueId,
                                                const std::string& id)
    {
        /*  ASKED HERE AS WELL AS AT THE DOOR, for createCanvas's reason: the
            container is made before the door is reached, and a locked show must
            not gain an empty one from a refusal. */
        if (auto refusal = refuseIfLocked())
            return *refusal;

        const auto cue = findById (cueId);

        if (! cue.isValid())
            return EditResult::failed (reason::unknownId);

        const auto kind = templateKindOf (cue);

        if (kind.empty())
            return EditResult::failed (reason::typeMismatch);

        const auto fragment = partFragmentOf (templateWordsFor (cue), cueId);

        if (fragment.empty())
            return EditResult::failed (reason::typeMismatch);

        auto container = showNode.getChildWithName ("CueTemplates");

        if (! container.isValid())
        {
            /*  AT A FIXED PLACE, after the video inputs, the video outputs,
                the canvases and the DCAs, whichever there are - and outside the history, as the
                canvases' container is: it carries nothing, and the template
                that made it is the step Undo takes back. */
            int at = 0;

            for (int i = 0; i < showNode.getNumChildren(); ++i)
            {
                const auto type = showNode.getChild (i).getType().toString();

                if (type == "Dcas" || type == "Canvases" || type == "VideoOutputs" || type == "VideoInputs"
                      || type == "VideoInserts")
                    at = i + 1;
            }

            container = juce::ValueTree ("CueTemplates");
            showNode.addChild (container, at, nullptr);
        }

        std::vector<std::pair<std::string_view, std::string>> attributes { { "kind", kind }, { "parts", fragment } };

        if (! name.empty())
            attributes.push_back ({ "name", name });

        return insertObject (container, endOfSequence, "CueTemplate", id, attributes);
    }

    EditResult ShowDocument::saveCueTemplate (const std::string& templateId, const std::string& cueId)
    {
        if (auto refusal = refuseIfLocked())
            return *refusal;

        auto found = findById (templateId);
        const auto cue = findById (cueId);

        if (! found.hasType ("CueTemplate") || ! cue.isValid())
            return EditResult::failed (reason::unknownId);

        /*  A TEMPLATE KEEPS ITS KIND: a movie's settings saved into a fill's
            template would offer a fill that is born a movie's. */
        if (templateKindOf (cue) != found.getProperty ("kind").toString().toStdString())
            return EditResult::failed (reason::typeMismatch);

        const auto fragment = partFragmentOf (templateWordsFor (cue), cueId);

        if (fragment.empty())
            return EditResult::failed (reason::typeMismatch);

        return writeOwned (found, "CueTemplate", "parts", fragment);
    }

    EditResult ShowDocument::applyCueTemplate (const std::string& templateId, const std::vector<std::string>& cueIds,
                                               const std::vector<std::string>& ids)
    {
        const auto found = findById (templateId);

        if (! found.hasType ("CueTemplate"))
            return EditResult::failed (reason::unknownId);

        return pastePart (found.getProperty ("parts").toString().toStdString(), cueIds, ids);
    }

    EditResult ShowDocument::createCueFrom (const std::string& parentId, int index, const std::string& templateId,
                                            const std::string& name, const std::string& id,
                                            const std::vector<std::string>& childIds, const Attributes& attributes,
                                            std::string& madeChildren)
    {
        if (auto refusal = refuseIfLocked())
            return *refusal;

        const auto found = findById (templateId);

        if (! found.hasType ("CueTemplate"))
            return EditResult::failed (reason::unknownId);

        const auto kind = found.getProperty ("kind").toString().toStdString();
        auto born = attributes;

        /*  A PICTURE'S SOURCE IS THE TEMPLATE'S KIND, unless a pair says so:
            the picture part leaves what a picture IS to the cue. */
        if (kind != "media"
              && std::none_of (born.begin(), born.end(), [] (const auto& pair) { return pair.first == "source"; }))
            born.insert (born.begin(), { "source", kind });

        const auto made = createCue (parentId, index, kind == "media" ? "media" : "video", name, id, born);

        if (! made.ok)
            return made;

        const auto stamped = pastePart (found.getProperty ("parts").toString().toStdString(), { made.id }, childIds);

        if (! stamped.ok)
            return stamped;

        madeChildren = stamped.id;

        /*  AND THE PAIRS GIVEN WIN: written again where the template wrote
            over them - a file's direct out, its channels - in the same step. */
        const auto cue = findById (made.id);

        for (const auto& [row, text] : born)
        {
            const auto address = "/godot/" + std::string (addressOwnerFor (cue.getType().toString().toStdString()))
                                   + "/" + made.id + "/" + row;

            if (getAttribute (address) == text)
                continue;

            if (const auto written = setAttribute (address, text); ! written.ok)
                return written;
        }

        return made;
    }
}
