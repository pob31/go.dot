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

#include <wfg/engine/tree/ParameterTree.h>
#include <wfg/engine/cue/FxRows.h>
#include <wfg/engine/cue/ShowWalk.h>

#include <wfg/engine/midi/PortTable.h>
#include <wfg/engine/surface/SurfaceTable.h>
#include <wfg/engine/cue/DcaTable.h>

#include <wfg/engine/cue/Solver.h>

#include <cctype>

#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/Sequence.h>

#include <wfg/engine/audio/MediaInfo.h>
#include <wfg/engine/clock/TickClock.h>
#include <wfg/engine/audio/Timbre.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

namespace wfg::tree
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        constexpr std::string_view rootAddress = "/";
        constexpr std::string_view godot = "/godot";

        Access accessFor (doc::Access access) noexcept
        {
            switch (access)
            {
                case doc::Access::read:      return Access::read;
                case doc::Access::write:     return Access::write;
                case doc::Access::readWrite: return Access::readWrite;
            }

            return Access::read;
        }

        Kind kindFor (doc::Kind kind) noexcept
        {
            switch (kind)
            {
                case doc::Kind::state: return Kind::state;
                case doc::Kind::event: return Kind::event;
            }

            return Kind::state;
        }

        /*  A document value as the wire carries it.

            Through the schema's own parser rather than reading the property
            directly, so the tree and the file agree about what a value IS by
            construction: the canonical text is the one representation both go
            through. The type tag decides f from d, since the table says which
            width a number travels at and OSC has no single "number". */
        std::optional<osc::Value> toOscValue (const doc::Attribute& attribute,
                                              const std::string& text)
        {
            doc::Value parsed;

            if (! doc::Schema::parseValue (attribute, text, parsed).ok)
                return std::nullopt;

            /*  Switched on the TABLE'S TYPE TAG, not on the parsed value's
                type, and the difference is not cosmetic. doc::Value has one
                integer, so a row declared `h` and a row declared `i` both parse
                to the same thing - and the tick index, which is `h` because it
                counts to more than two billion, would go on the wire as an
                int32 and wrap after about five hundred days of running.

                The table says how wide a value travels. This is the only place
                that has to know it, and it reads the declaration rather than
                guessing from the value. */
            switch (attribute.oscTypeTag())
            {
                case 's': return osc::Value::string (parsed.getString());
                case 'i': return osc::Value::int32 (static_cast<std::int32_t> (parsed.getInteger()));
                case 'h': return osc::Value::int64 (parsed.getInteger());
                case 'f': return osc::Value::float32 (static_cast<float> (parsed.getNumber()));
                case 'd': return osc::Value::float64 (parsed.getNumber());
                case 'T': return osc::Value::boolean (parsed.getBoolean());

                default:
                    /*  A blob, or a tag nothing declares. Returning nothing
                        keeps the node valueless, which is at least honest about
                        what the tree knows. */
                    return std::nullopt;
            }
        }

        /*  The tokens of a list attribute, which is XSD's list lexical form:
            values separated by whitespace, leading and trailing ignored, and an
            empty string is zero values rather than one empty one. */
        std::vector<std::string> splitList (const std::string& text)
        {
            std::vector<std::string> out;
            std::size_t i = 0;

            while (i < text.size())
            {
                while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) != 0)
                    ++i;

                const auto start = i;

                while (i < text.size() && std::isspace (static_cast<unsigned char> (text[i])) == 0)
                    ++i;

                if (i > start)
                    out.push_back (text.substr (start, i - start));
            }

            return out;
        }

        /*  Every value the attribute holds: one for a scalar, one per element
            for a list, and none at all when the text does not parse.

            A LIST THAT FAILS ANYWHERE PUBLISHES NOTHING, rather than the
            elements that happened to parse. Half a routing matrix is not a
            smaller routing matrix, it is a different one, and a client given
            three of four gains has been told something untrue about where a cue
            goes. The document validator refuses such a file at load; this is
            the same answer one layer up. */
        std::vector<osc::Value> toOscValues (const doc::Attribute& attribute,
                                             const std::string& text)
        {
            if (! attribute.isList())
            {
                if (auto single = toOscValue (attribute, text))
                    return { *single };

                return {};
            }

            std::vector<osc::Value> out;

            for (const auto& token : splitList (text))
            {
                auto element = toOscValue (attribute, token);

                if (! element.has_value())
                    return {};

                out.push_back (*element);
            }

            return out;
        }

        Node makeContainer (std::string address)
        {
            Node node;
            node.address = std::move (address);
            node.kind = Kind::container;
            node.access = Access::none;
            return node;
        }

        /*  A leaf built from one row of the parameter table, plus the text of
            its current value. Everything a client is told about this node comes
            from the row - the type, the access, the range, the unit, the
            description and all four GODOT declarations - so there is no second
            place for any of it to be wrong. */
        Node makeLeaf (std::string address, const doc::AttributeRow& row,
                       const std::string& valueText)
        {
            /*  The element label is the row's own owner word. Nothing in the
                value path reads it - parseValue works entirely off the row -
                and passing a document element name here would only invite
                someone to believe otherwise. */
            const doc::Attribute attribute { row.owner, &row };

            Node node;
            node.address = std::move (address);
            node.kind = kindFor (row.kind);
            node.access = accessFor (row.access);
            node.typeTags = std::string (1, row.oscTypeTag);   // widened below for a list
            node.description = std::string (row.description);

            node.hasMinimum = row.hasMin;
            node.minimum = row.minimum;
            node.hasMaximum = row.hasMax;
            node.maximum = row.maximum;
            node.unit = std::string (row.unit);

            for (std::size_t i = 0; i < row.numEnumValues; ++i)
                node.enumValues.push_back (std::string (row.enumValues[i]));

            node.rateCap = row.rateCap;
            node.anticipatable = row.anticipatable;
            node.panic = std::string (row.panic);

            /*  An event has no value at a given time (PRD §3.3), so it is given
                none. A nil or a zero would be an answer to a question that has
                none. */
            /*  A LIST NODE PUBLISHES ONE VALUE PER ELEMENT, and its TYPE
                string grows to match - `ddd` for three gains. That is ordinary
                OSCQuery: TYPE is per node, and a node carrying a run of numbers
                says so the same way a command carrying three arguments does.

                It means the type string of a list node depends on its value,
                which is unusual and worth naming. It is stable in practice:
                `Route/@gains` is C_in x width, and both come from the document,
                so it changes only when the show does. */
            if (node.kind != Kind::event)
            {
                node.values = toOscValues (attribute, valueText);
                node.typeTags = std::string (node.values.size(), row.oscTypeTag);
            }

            return node;
        }

        /*  The stored text of an attribute, or its default when the document
            does not carry it - which is the same rule the canonical writer uses
            in the other direction, and the reason a sparse file round-trips. */
        std::string storedText (const doc::Attribute& attribute, const juce::ValueTree& node)
        {
            /*  Through the canonical writer's own formatter, and not through
                juce::var::toString(): that would put a double through JUCE's
                number writer, which loses 46% of them to a round trip
                (measured; the table is in osc/OscValue.cpp). The tree would
                then publish a different number from the one in the file.

                attributeText returns nothing when the node does not carry the
                attribute OR carries exactly its default, which are the same
                thing to a reader - so both fall through to the default here. */
            if (auto text = doc::CanonicalXml::attributeText (attribute, node))
                return *text;

            return std::string (attribute.defaultText());
        }

        /*  The identifiers of a container's members, in order, space-separated.

            `only` narrows it to one element name, which is what a group's
            header and footer need: `order` is the group's MEMBERS and a header
            is not one of them - a client reading `order` is reading the cue
            list, and the two cues that run before and after it are a different
            question with two nodes of their own.

            WHICH CHILD IS A MEMBER IS NO LONGER DECIDED HERE. The predicate
            used to be written out on the line below, and it was the only
            statement of it anywhere - so when the document's `object.move` and
            `cue.create` came to read their index as a position in THIS
            sequence, the choice was to copy it into the door or to lift it out.
            Two copies of one rule is how a publisher and a door come to
            disagree about what a show says, so it lives in
            `document/Sequence.h` and this asks it like anyone else. */
        std::string orderOf (const juce::ValueTree& node, const char* only = nullptr)
        {
            std::string out;

            for (const auto& child : node)
            {
                if (! child.hasProperty (idProperty))
                    continue;

                if (only != nullptr ? child.getType().toString() != only
                                    : ! doc::isSequenceChild (child))
                    continue;

                if (! out.empty())
                    out += ' ';

                out += child[idProperty].toString().toStdString();
            }

            return out;
        }

        /*  Every container an address implies, added once, so nothing has to
            emit `/godot/cue` by hand and then remember to keep it in step with
            the leaves underneath it.

            `ownedElsewhere` is the small set of containers the OTHER half of
            the snapshot is responsible for. The two halves must not both carry
            an address: `find` searches one and then the other, and a duplicate
            would mean the answer depended on which it happened to look at
            first. */
        void addContainers (std::vector<Node>& nodes,
                            const std::vector<std::string>& ownedElsewhere,
                            bool includeRoot)
        {
            std::set<std::string> present;

            for (const auto& node : nodes)
                present.insert (node.address);

            for (const auto& address : ownedElsewhere)
                present.insert (address);

            std::set<std::string> wanted;

            for (const auto& node : nodes)
            {
                auto address = node.address;

                for (;;)
                {
                    const auto slash = address.rfind ('/');

                    if (slash == std::string::npos || slash == 0)
                        break;

                    address = address.substr (0, slash);
                    wanted.insert (address);
                }
            }

            if (includeRoot)
                wanted.insert (std::string (rootAddress));

            for (const auto& address : wanted)
                if (present.find (address) == present.end())
                    nodes.push_back (makeContainer (address));
        }

        void sortByAddress (std::vector<Node>& nodes)
        {
            std::sort (nodes.begin(), nodes.end(),
                       [] (const Node& a, const Node& b) { return a.address < b.address; });
        }

        //======================================================================
        /*  A cue or a group, and everything under it.

            `parentId` and `index` are passed down rather than looked up because
            they are exactly the two derived values the table declares and the
            document refuses to store: the structure already says them. */
        /*  One destination of one media cue, at an address of its own.

            It carries no `parent` or `index` row: a route belongs to exactly
            one cue, which the document says by containment, and it has no
            position anybody can act on. What it needs is a bus and a run of
            coefficients. */
        /*  The set entry a cue's Fx names, and where it sits in the chain: read
            off the document root, which every node can reach. */
        struct SetEntry
        {
            juce::ValueTree element;
            int index = -1;
        };

        SetEntry setEntryFor (const juce::ValueTree& anyNode, const std::string& pluginId)
        {
            const auto plugins = anyNode.getRoot().getChildWithName ("Audio").getChildWithName ("Plugins");
            auto index = 0;

            for (const auto entry : plugins)
            {
                if (! entry.hasType ("Plugin"))
                    continue;

                if (entry[idProperty].toString().toStdString() == pluginId)
                    return { entry, index };

                ++index;
            }

            return {};
        }

        bool fxIsEnabled (const juce::ValueTree& fx)
        {
            /*  Through the schema: the row's default is true and the canonical
                writer omits a default, and a stored flag is a bool var, not
                the word - both of which the Reader knows. */
            static const cue::Reader schema;
            return schema.flag (fx, "fx", "enabled");
        }

        /*  The cue's enabled inserts in CHAIN order - the set's order, not the
            children's - for `media/fx` (pages draft §8, item 3). */
        std::string enabledFxInChainOrder (const juce::ValueTree& cue)
        {
            std::vector<std::pair<int, std::string>> found;

            for (const auto child : cue)
            {
                if (! child.hasType ("Fx") || ! fxIsEnabled (child))
                    continue;

                const auto entry = setEntryFor (cue, child["plugin"].toString().toStdString());

                if (entry.index >= 0)
                    found.emplace_back (entry.index, child[idProperty].toString().toStdString());
            }

            std::sort (found.begin(), found.end());
            std::string out;

            for (const auto& [index, fxId] : found)
            {
                if (! out.empty())
                    out += ' ';

                out += fxId;
            }

            return out;
        }

        /*  One insert of one media cue, at an address of its own (Phase 9a). */
        void collectFx (const juce::ValueTree& fx, const std::string& cueId, std::vector<Node>& out)
        {
            const auto fxId = fx[idProperty].toString().toStdString();

            if (fxId.empty())
                return;

            const auto entry = setEntryFor (fx, fx["plugin"].toString().toStdString());
            const auto base = std::string (godot) + "/fx/" + fxId;

            for (const auto* row : doc::Schema::rowsForOwner ("fx"))
            {
                const doc::Attribute attribute { "Fx", row };
                const auto name = std::string (row->name);
                std::string text;

                if (name == "cue")        text = cueId;
                else if (name == "name")  text = entry.element.isValid() ? entry.element["name"].toString().toStdString() : std::string {};
                else if (name == "index") text = std::to_string (std::max (0, entry.index));
                else                      text = storedText (attribute, fx);

                out.push_back (makeLeaf (base + "/" + name, *row, text));
            }
        }

        void collectRoute (const juce::ValueTree& node, std::vector<Node>& out)
        {
            const auto id = node[idProperty].toString().toStdString();

            if (id.empty())
                return;

            const auto base = std::string (godot) + "/route/" + id;

            for (const auto* row : doc::Schema::rowsForOwner ("route"))
            {
                const doc::Attribute attribute { "Route", row };

                out.push_back (makeLeaf (base + "/" + std::string (row->name),
                                         *row, storedText (attribute, node)));
            }
        }

        /*  A RANGE, at a top level address of its own.

            The Route and Trigger precedent again, with one difference that
            matters: a range HAS a position and the position is the playlist.
            So `index` is published beside `cue`, and both are derived - the
            containment says which cue, the sibling order says which pass. A
            stored copy of either could come to disagree with the document, and
            the strip's "3 of 8" would then be a number about nothing. */
        void collectRange (const juce::ValueTree& node, const std::string& cueId,
                           int index, std::vector<Node>& out)
        {
            const auto id = node[idProperty].toString().toStdString();

            if (id.empty())
                return;

            const auto base = std::string (godot) + "/range/" + id;

            for (const auto* row : doc::Schema::rowsForOwner ("range"))
            {
                const doc::Attribute attribute { "Range", row };
                const auto name = std::string (row->name);

                const auto text = name == "cue"   ? cueId
                                : name == "index" ? std::to_string (index)
                                                  : storedText (attribute, node);

                out.push_back (makeLeaf (base + "/" + name, *row, text));
            }
        }

        /*  A TRIGGER, at a top level address of its own.

            Flat rather than nested under the cue, which is the Route precedent
            and the same argument: a client watching one trigger is watching one
            node, and a trigger belongs to exactly one cue - so `cue` is derived
            from where it sits rather than stored, and the containment cannot
            come to disagree with a copy of itself. */
        void collectTrigger (const juce::ValueTree& node, const std::string& cueId,
                             std::vector<Node>& out)
        {
            const auto id = node[idProperty].toString().toStdString();

            if (id.empty())
                return;

            const auto base = std::string (godot) + "/trigger/" + id;

            for (const auto* row : doc::Schema::rowsForOwner ("trigger"))
            {
                const doc::Attribute attribute { "Trigger", row };
                const auto name = std::string (row->name);

                const auto text = name == "cue" ? cueId : storedText (attribute, node);
                out.push_back (makeLeaf (base + "/" + name, *row, text));
            }
        }

        /*  `role` is where this cue sits in the thing that contains it, and it
            is a parameter rather than something read off the node because the
            node cannot know: a Media element is the same element whether it is
            a member, a header cue or a footer cue, and what differs is only
            which branch below recursed into it. */
        /*  ONE DECLARED SLOT, wherever the document keeps it.

            §1's first rule applies to a slot as it does to a cue: objects are
            identity-addressed, so a `Slot` under a mount and a `Channel` under
            the rack both live at `/godot/slot/<id>` and a client holding an
            identifier never has to know which container it came out of. What
            differs is the rows, and that is what the second owner is for - the
            `Media` shape exactly, where a media cue is a cue first.

            A VOICE IS NOT HERE. A track is not an object anybody declared: no
            identifier, and `@tracks` is a count rather than a list. Inventing
            `/godot/slot/voice3` would force one address to be `persist=show`
            for a declared slot and `none` for a track, which no row can be.
            Where a voice is held has been readable at `/godot/run/<id>/track`
            since Phase 2. */
        /** One row of one owner, or nullptr. */
        const doc::AttributeRow* rowNamed (std::string_view owner, std::string_view name)
        {
            for (const auto* row : doc::Schema::rowsForOwner (owner))
                if (row->name == name)
                    return row;

            return nullptr;
        }

        void collectSlot (const juce::ValueTree& node, const char* element,
                          const char* extraOwner, const char* kindText,
                          const cue::SlotAnalysis& analysis,
                          std::vector<Node>& out,
                          const std::map<std::string, std::string>* derived = nullptr)
        {
            const auto id = node[idProperty].toString().toStdString();

            if (id.empty())
                return;

            const auto base = std::string (godot) + "/slot/" + id;

            auto rows = doc::Schema::rowsForOwner ("slot");

            for (auto* row : doc::Schema::rowsForOwner (extraOwner))
                rows.push_back (row);

            for (const auto* row : rows)
            {
                const doc::Attribute attribute { element, row };
                const auto name = std::string (row->name);

                /*  Derived from the element that holds it, never stored - the
                    `cue/kind` rule, and for the same reason: a client that
                    could write it could turn a processor input into a rack
                    channel by writing a word, and the two are released and
                    refused by different policies. */
                /*  `holder` and `pending` are NOT emitted here. They change
                    with every run while nothing about the show does, and this
                    half is a cache - published from here they would freeze at
                    whatever they were when a cue was last edited. The runtime
                    half emits them, against the roster this walk leaves behind. */
                if (name == "holder" || name == "pending")
                    continue;

                /*  A STRIP'S LIVE ROWS, for the same reason (Phase 6): what it
                    is riding, the word its display shows and the cue on it
                    change with every press and every handover while nothing
                    about the show does. The runtime half emits them. */
                if (name == "target" || name == "word" || name == "cue")
                    continue;

                /*  And what the caller computed from the structure - a
                    strip's surface, its place on it and what kind of hand
                    control it is - which the element does not store. */
                if (derived != nullptr)
                    if (const auto found = derived->find (name); found != derived->end())
                    {
                        out.push_back (makeLeaf (base + "/" + name, *row, found->second));
                        continue;
                    }

                /*  `usage` and `overlaps` ARE from here, and the difference is
                    what they are about. They change when somebody edits the
                    show and at no other time - they are a reading of the
                    document, exactly as `name` and `width` are - so the cached
                    half is where they belong. What they are read out of is a
                    cache of their own, because the walk behind them is the
                    whole show and this half rebuilds on a cue rename. */
                const auto text = name == "kind"     ? std::string (kindText)
                                : name == "usage"    ? analysis.usageOf (id)
                                : name == "overlaps" ? analysis.overlapsOf (id)
                                                     : storedText (attribute, node);

                out.push_back (makeLeaf (base + "/" + name, *row, text));
            }
        }

        /*  THE CUES WHOSE `preset` NAMES THIS GROUP, in document order.

            §13.7's derived header line, and it is a walk of the group's own
            subtree rather than of the show: a `preset` names an ANCESTOR, so
            every cue that could name this group is somewhere underneath it.
            A value naming a group that is not an ancestor is a `wfg validate`
            warning and is ignored, which is what makes that true here.

            Derived the way `order` and `headerOrder` are - nothing is copied
            into the `Header` element, so editing the line is editing the member
            and the two can never come to disagree. */
        void collectPresets (const juce::ValueTree& node, const std::string& groupId,
                             std::vector<std::string>& out)
        {
            for (const auto& child : node)
            {
                if (! child.hasProperty (idProperty))
                    continue;

                const auto element = child.getType().toString().toStdString();

                if (element == "Header" || element == "Footer")
                {
                    collectPresets (child, groupId, out);
                    continue;
                }

                if (doc::ShowDocument::ownerForElement (element) != "cue")
                    continue;

                if (child[juce::Identifier ("preset")].toString().toStdString() == groupId)
                    out.push_back (child[idProperty].toString().toStdString());

                collectPresets (child, groupId, out);
            }
        }

        /*  `analysis` is here for the two media rows that say which outputs
            are busy where this cue plays. Handed down rather than looked up,
            because this walk is already the one place that knows which cue it
            is at, and a second lookup would be a second thing to keep in step
            with the first. */
        void collectCue (const juce::ValueTree& node, const std::string& parentId, int index,
                         std::vector<Node>& out,
                         const std::map<std::string, double>* durations,
                         std::vector<std::string>& roster,
                         std::vector<std::pair<std::string, std::string>>& mediaRoster,
                         const cue::SlotAnalysis& analysis,
                         const char* role = "member")
        {
            const auto element = node.getType().toString().toStdString();
            const auto isGroup = element == "Group";
            const auto isMedia = element == "Media";
            const auto isFade = element == "Fade";
            const auto isStop = element == "Transport";
            const auto isOsc = element == "Osc";
            const auto isMidi = element == "Midi";
            const auto isStart = element == "Start";
            const auto id = node[idProperty].toString().toStdString();

            if (id.empty())
                return;

            const auto base = std::string (godot) + "/cue/" + id;

            /*  KEPT FOR THE RUNTIME HALF, which has no document to walk. See
                `ParameterTree::declaredCues`. */
            roster.push_back (id);

            /*  And a media cue's file beside it, read the way MediaInfo keys
                its records - the raw attribute, as `mediaFilesNamedBy` reads
                it - so the hash the runtime half looks up is the file this cue
                names and not a spelling of it. See
                `ParameterTree::declaredMedia`. */
            if (isMedia)
                mediaRoster.emplace_back (id, node[juce::Identifier ("file")].toString().toStdString());

            /*  EVERY KIND IS A CUE FIRST. A media cue has a number, a name and
                a pre-wait like any other and is addressed at /godot/cue/<id>,
                so a client holding an identifier never has to know which kind
                it got. The kind's own rows are appended, which is the same
                thing a Group does with the `group` owner. */
            auto rows = doc::Schema::rowsForOwner ("cue");

            if (isGroup)
                for (auto* row : doc::Schema::rowsForOwner ("group"))
                    rows.push_back (row);

            if (isMedia)
                for (auto* row : doc::Schema::rowsForOwner ("media"))
                    rows.push_back (row);

            if (isFade)
                for (auto* row : doc::Schema::rowsForOwner ("fade"))
                    rows.push_back (row);

            if (isStop)
                for (auto* row : doc::Schema::rowsForOwner ("transport"))
                    rows.push_back (row);

            if (isOsc)
                for (auto* row : doc::Schema::rowsForOwner ("osc"))
                    rows.push_back (row);

            if (isMidi)
                for (auto* row : doc::Schema::rowsForOwner ("midi"))
                    rows.push_back (row);

            if (isStart)
                for (auto* row : doc::Schema::rowsForOwner ("start"))
                    rows.push_back (row);

            for (const auto* row : rows)
            {
                const doc::Attribute attribute { element, row };
                const auto name = std::string (row->name);

                /*  `prepare` is NOT emitted here. How far ahead a cue has been
                    got ready changes as the pointer moves while nothing about
                    the show does, and this half is a cache - published from
                    here it would freeze at whatever it was when a cue was last
                    edited. The runtime half emits it, against the roster this
                    walk leaves behind. Same rule as a slot's `holder`. */
                if (name == "prepare")
                    continue;

                /*  NOR IS A MEDIA CUE'S `hash`, for the same reason with a
                    different clock. It arrives when the analyser has read the
                    file - seconds or minutes after the show opened, on a thread
                    of its own - and nothing about the show moves when it does,
                    so published from here it would read empty until somebody
                    happened to edit a cue. The runtime half emits it against
                    the media roster this walk leaves behind, and only there:
                    both halves carrying the address would make the answer
                    depend on which one `find` reached first (§14.5). */
                if (row->owner == "media" && name == "hash")
                    continue;

                std::string text;

                /*  Derived from the element, never stored - which is what makes
                    it read-only in a way a client cannot argue with. */
                if (name == "kind")        text = isGroup ? "group"
                                                  : isMedia ? "media"
                                                  : isFade  ? "fade"
                                                  : isStop  ? "transport"
                                                  : isOsc   ? "osc"
                                                  : isMidi  ? "midi"
                                                  : isStart ? "start"
                                                            : "memo";
                else if (name == "parent") text = parentId;
                else if (name == "index")  text = std::to_string (index);
                else if (name == "role")   text = role;
                else if (name == "fx" && isMedia) text = enabledFxInChainOrder (node);
                else if (name == "duration" && isMedia)
                {
                    /*  READ ONCE WHEN THE SHOW WAS OPENED, and nought when
                        nobody read any: a replay, a tree dump of a bundle with
                        no media folder, a test. Nought is the same answer a
                        missing or unreadable file gives, and §3.13's solver
                        reads it as "I do not know how long this is" rather than
                        as "this has ended".

                        A MEDIA CUE'S ONLY, and the guard is a fix (PR 5.8).
                        `fade` and `stop` carry a `duration` row of their own -
                        a length somebody DECIDED, rw and stored - and without
                        it this branch answered theirs from the media table too,
                        keyed by a `file` they do not have: every fade and stop
                        published a duration of nought whatever the show said,
                        since PR 4.1. Theirs falls through to the stored text. */
                    const auto named = node[juce::Identifier ("file")].toString().toStdString();
                    const auto found = durations != nullptr ? durations->find (named)
                                                            : std::map<std::string, double>::const_iterator {};

                    text = (durations != nullptr && found != durations->end())
                             ? osc::formatDouble (found->second)
                             : osc::formatDouble (0.0);
                }
                else if (name == "outsBusy" && isMedia)
                {
                    /*  WHICH OUTPUTS ARE TAKEN WHERE THIS CUE PLAYS, from the
                        same revision-keyed cache the slot rows come from.

                        PUBLISHED PER CUE AND NOT PER BUS, and the asymmetry is
                        the question rather than a preference. `bus/usage`
                        answers "who is on this output"; a menu asks "what is
                        in the way of THIS cue", and a client cannot work the
                        second out of the first - it would have to reconstruct
                        row order across nested groups, headers and footers,
                        and it could not compute the seconds rule at all. That
                        is re-implementing `ShowWalk` in a client, which is
                        what the boundary check exists to prevent. */
                    text = analysis.busyOutsOf (id);
                }
                else if (name == "outsMaybe" && isMedia)
                {
                    text = analysis.maybeOutsOf (id);
                }
                else if (name == "headerDerived")
                {
                    std::vector<std::string> derived;
                    collectPresets (node, id, derived);

                    for (const auto& derivedId : derived)
                    {
                        if (! text.empty())
                            text += ' ';

                        text += derivedId;
                    }
                }
                else if (name == "order")  text = orderOf (node);
                else if (name == "headerOrder")
                    text = orderOf (node.getChildWithName ("Header"));
                else if (name == "footerOrder")
                    text = orderOf (node.getChildWithName ("Footer"));
                /*  THE SECTIONS' OWN NAMES, as the list's `persistent` is: what
                    a client hands `object.move` to put a cue in one. Empty
                    until `group.role` has made the section. */
                else if (name == "header" || name == "footer")
                {
                    const auto section = node.getChildWithName (name == "header" ? "Header" : "Footer");
                    text = section.isValid() ? section[idProperty].toString().toStdString() : std::string {};
                }
                else                       text = storedText (attribute, node);

                out.push_back (makeLeaf (base + "/" + name, *row, text));
            }

            int childIndex = 0;
            int rangeIndex = 0;

            for (const auto& child : node)
            {
                if (! child.hasProperty (idProperty))
                    continue;

                /*  NOT EVERY IDENTIFIED CHILD IS A NESTED CUE, and the
                    recursion below takes any that reaches it - so the element
                    has to be asked what it is.

                    A ROUTE has a top-level address of its own, so that changing
                    one gain is a write to one node rather than a rewrite of the
                    cue's whole routing (author, 2026-09-05).

                    A HEADER OR A FOOTER is an ordinary cue list (§3.6) and
                    carries nothing but an identifier, so it publishes no node of
                    its own - but the cues INSIDE it are cues, published like any
                    other, with `role` saying where in the group they sit. What
                    it must not do is take an index among the members: a header
                    is not the group's first member, and letting it have index 0
                    would have shifted every real member by one.

                    Written as a lookup rather than as a second `if` because the
                    list is now three long and the plan has ranges and triggers
                    joining it - and because the failure mode of forgetting one
                    is silent: it becomes a cue at `/godot/cue/<id>`, with the
                    rows of a kind it is not. */
                const auto childElement = child.getType().toString();

                if (childElement == "Route")
                {
                    collectRoute (child, out);
                    continue;
                }

                /*  A DESTINATION IS NOT A NESTED CUE. The recursion at the foot
                    of this loop takes anything that reaches it, so a `Feed`
                    left unlisted would be published at `/godot/cue/<id>` with
                    the rows of a kind it is not - which is the failure mode the
                    comment above names, and the reason this is a lookup. */
                /*  AN INSERT (Phase 9a, PR 9a.8): its rows at /godot/fx/<id>, the
                    name and the index read off the set. Its parameter nodes are
                    a pass of their own, after the walk, because they need the
                    catalogue and this walk has no reach to it. */
                if (childElement == "Fx")
                {
                    collectFx (child, id, out);
                    continue;
                }

                if (childElement == "Feed" || childElement == "Insert"
                      || childElement == "Send")
                {
                    const auto* owner = childElement == "Feed"   ? "feed"
                                      : childElement == "Insert" ? "insert"
                                                                 : "send";

                    /*  A LITERAL, because `Attribute::element` is a view. It was
                        built from `childElement.toStdString()`, a temporary that
                        died at the end of its own line and left the view
                        pointing at freed memory for every row below. Nothing
                        read it yet - attributeText asks the row, not the
                        element - which is why it only showed as a warning
                        (clang's dangling-gsl), and why it is fixed before the
                        first reader of `element` finds it the other way. */
                    const auto* elementName = childElement == "Feed"   ? "Feed"
                                            : childElement == "Insert" ? "Insert"
                                                                       : "Send";
                    const auto childId = child[idProperty].toString().toStdString();

                    if (! childId.empty())
                    {
                        const auto childBase = std::string (godot) + "/"
                                                 + std::string (owner) + "/" + childId;

                        for (const auto* row : doc::Schema::rowsForOwner (owner))
                        {
                            const doc::Attribute attribute { elementName, row };
                            const auto name = std::string (row->name);
                            const auto text = name == "cue" ? id
                                                            : storedText (attribute, child);

                            out.push_back (makeLeaf (childBase + "/" + name, *row, text));
                        }
                    }

                    continue;
                }

                if (childElement == "Range")
                {
                    /*  Counted separately from the cue index, because a range
                        is not a member of anything: its number is its place in
                        this cue's playlist, and a Route or a Trigger sitting
                        between two ranges must not move it. */
                    collectRange (child, id, rangeIndex++, out);
                    continue;
                }

                if (childElement == "Trigger")
                {
                    /*  A TRIGGER IS IN `order` AND NOT IN THIS COUNT, which is
                        a disagreement worth writing down rather than leaving
                        for whoever meets it.

                        It carries an identifier and is not a header, a footer
                        or a section, so `orderOf` lists it among the parent's
                        members - while the walk here passes over it before
                        `childIndex` is spent, so the `index` every cue beneath
                        it publishes counts cues only. On a group that holds a
                        trigger the two therefore differ by one from the trigger
                        down.

                        Nothing is changed about it here, deliberately. The
                        index in `object.move` and `cue.create` is read against
                        the sequence a CLIENT can see, which is `order`
                        (`document/Sequence.h`), so the door and the publisher
                        agree with each other whatever this count says. Making
                        all three agree means deciding whether a trigger is a
                        member at all - a question about the namespace, not
                        about a walk, and one with its own PR to come. */
                    collectTrigger (child, id, out);
                    continue;
                }

                if (childElement == "Header" || childElement == "Footer")
                {
                    int roleIndex = 0;
                    const auto* childRole = childElement == "Header" ? "header" : "footer";

                    for (const auto& roleChild : child)
                        if (roleChild.hasProperty (idProperty))
                            collectCue (roleChild, id, roleIndex++, out, durations, roster,
                                        mediaRoster, analysis, childRole);

                    continue;
                }

                collectCue (child, id, childIndex++, out, durations, roster, mediaRoster,
                            analysis);
            }
        }
    }

    //==============================================================================
    ParameterTree::ParameterTree (const doc::ShowDocument& documentToProject,
                                  const CommandRegistry& commandsToDescribe,
                                  const MountTable& mountsToPublish,
                                  const cue::RunTable& runsToPublish)
        : document (documentToProject),
          commands (commandsToDescribe),
          mounts (mountsToPublish),
          runs (runsToPublish)
    {
    }

    //==============================================================================
    void ParameterTree::rebuildDocumentPart()
    {
        std::vector<Node> nodes;

        const auto showNode = document.root();

        //----------------------------------------------------------------------
        // /godot/document — the rows that persist, read off the root: the
        // show format's own version, and the edit lock, which persists in
        // state.xml rather than show.xml and is published from here all the
        // same. The split is the persist column and not how often a value
        // changes. The rows that persist nowhere are runtime state and live on
        // the other side.
        for (const auto* row : doc::Schema::rowsForOwner ("document"))
        {
            if (row->persist == doc::Persist::none)
                continue;

            const doc::Attribute attribute { doc::Schema::rootElement, row };

            nodes.push_back (makeLeaf (std::string (godot) + "/document/" + std::string (row->name),
                                       *row, storedText (attribute, showNode)));
        }

        //----------------------------------------------------------------------
        /*  Every declared slot, in the order the document keeps them: a mount's
            processor inputs first, because mounts are written before the audio
            section, then the rack's channels. Gathered while the containers are
            walked rather than by a second traversal. */
        std::vector<std::string> slotOrder;

        /*  And every cue, for the same reason and out of the same walk: the
            runtime half publishes `prepare` against it. See
            `ParameterTree::declaredCues`. */
        std::vector<std::string> cueOrder;

        /*  And every media cue with the file it names, for `hash`. See
            `ParameterTree::declaredMedia`. */
        std::vector<std::pair<std::string, std::string>> mediaOrder;

        /*  And every list, for `aim`, `solve` and `statePosition` - three more
            answers that are about a session rather than about a show. */
        std::vector<std::string> listOrder;

        /*  And every DCA, for its `trim` - what a fader is doing tonight, which
            the runtime half publishes. */
        std::vector<std::string> dcaOrder;
        std::vector<std::string> pluginOrder;

        /*  And every strip, for what it is riding and its word. */
        std::vector<DeclaredStrip> stripOrder;

        for (const auto& container : showNode)
        {
            const auto containerName = container.getType().toString().toStdString();

            if (containerName == "Lists")
            {
                /*  THE COLLECTION'S OWN NODES, which §2.3 of the namespace draft
                    promised in Phase 1 and could not have then: they need a
                    parameter-table owner for the container itself, and there was
                    one list, so there was nothing for a focus to be exclusive
                    about. Parallel lists give it something.

                    `/godot/list/order` is the roster and `/godot/list/focus` is
                    which of them GO acts on - both at the container address,
                    beside `/godot/list/<id>/standby`, because they are one thing
                    read two ways. */
                for (const auto* row : doc::Schema::rowsForOwner ("lists"))
                {
                    const doc::Attribute attribute { "Lists", row };
                    const auto name = std::string (row->name);
                    const auto text = name == "order" ? orderOf (container)
                                                      : storedText (attribute, container);

                    nodes.push_back (makeLeaf (std::string (godot) + "/list/" + name,
                                               *row, text));
                }

                for (const auto& list : container)
                {
                    const auto id = list[idProperty].toString().toStdString();

                    if (id.empty())
                        continue;

                    const auto base = std::string (godot) + "/list/" + id;

                    for (const auto* row : doc::Schema::rowsForOwner ("list"))
                    {
                        const doc::Attribute attribute { "List", row };
                        const auto name = std::string (row->name);

                        /*  `aim`, `solve` and `statePosition` are NOT emitted
                            here. None of them is about the show: an aim is
                            where somebody's finger is, a solve is the answer to
                            that question, and a state position is where a jump
                            landed - so all three change while nothing about the
                            document does, and this half is a cache. The runtime
                            half emits them, against the roster this walk leaves
                            behind. Same rule as a cue's `prepare` and a slot's
                            `holder`. */
                        if (name == "aim" || name == "solve" || name == "statePosition"
                             || name == "history")
                            continue;

                        const auto section = list.getChildWithName ("Persistent");

                        const auto text
                            = name == "order"           ? orderOf (list)
                            : name == "persistentOrder" ? orderOf (section)
                            : name == "persistent"      ? (section.isValid()
                                                             ? section[idProperty].toString()
                                                                                  .toStdString()
                                                             : std::string {})
                                                        : storedText (attribute, list);

                        nodes.push_back (makeLeaf (base + "/" + name, *row, text));
                    }

                    listOrder.push_back (id);

                    int index = 0;

                    for (const auto& cue : list)
                    {
                        /*  THE SECTION IS NOT A CUE, though it carries an
                            identifier so that `cue.create` can name it as a
                            parent. Its cues are collected after the members,
                            with the role that says where they sit. */
                        if (cue.getType().toString() == "Persistent")
                            continue;

                        if (cue.hasProperty (idProperty))
                            collectCue (cue, id, index++, nodes, durations, cueOrder, mediaOrder,
                                        analysis);
                    }

                    if (const auto section = list.getChildWithName ("Persistent");
                        section.isValid())
                    {
                        int persistentIndex = 0;

                        for (const auto& cue : section)
                            if (cue.hasProperty (idProperty))
                                collectCue (cue, id, persistentIndex++, nodes, durations,
                                            cueOrder, mediaOrder, analysis, "persistent");
                    }
                }
            }
            else if (containerName == "Mounts")
            {
                for (const auto& mount : container)
                {
                    const auto id = mount[idProperty].toString().toStdString();

                    if (id.empty())
                        continue;

                    const auto base = std::string (godot) + "/mount/" + id;

                    for (const auto* row : doc::Schema::rowsForOwner ("mount"))
                    {
                        const doc::Attribute attribute { "Mount", row };
                        const auto name = std::string (row->name);

                        /*  `loaded` and `nodeCount` describe what the engine did
                            with the mount rather than what the file says, so
                            they come from the mount table and not from the
                            document. They are the honest answer to "did that
                            actually work", which is the question somebody asks
                            when a target is not responding. */
                        std::string text;

                        if (name == "loaded")
                            text = mounts.isLoaded (id) ? "true" : "false";
                        else if (name == "nodeCount")
                            text = std::to_string (mounts.nodeCount (id));
                        else if (name == "sent")
                            text = std::to_string (sender != nullptr ? sender->sentFor (id) : 0u);
                        else if (name == "problem")
                            text = mounts.problemOf (id);
                        else
                            text = storedText (attribute, mount);

                        nodes.push_back (makeLeaf (base + "/" + name, *row, text));
                    }

                    /*  AND THE INPUTS THE SHOW USES OF IT (decision P). The
                        processor could describe its own - §3.9b marks that
                        *(proposed)* - and the author's decision is that the SHOW
                        declares them and the mounted namespace is what a
                        validate pass checks against. How many inputs of a
                        processor a show is using is something somebody decided
                        (§4.10), and a pool that changed when a processor was
                        reconfigured would change a show nobody had edited. */
                    for (const auto& slot : mount)
                    {
                        if (slot.getType().toString() != "Slot")
                            continue;

                        collectSlot (slot, "Slot", "processorInput", "processorInput",
                                     analysis, nodes);

                        if (const auto slotId = slot[idProperty].toString().toStdString();
                            ! slotId.empty())
                            slotOrder.push_back (slotId);
                    }
                }
            }
            else if (containerName == "MidiPorts")
            {
                /*  THE PORTS THE SHOW DECLARES, and which cable each one
                    turned out to be.

                    The NAME of the device is the show's, because it is what
                    somebody decided and what reads sensibly at another venue
                    (§4.10) - `audio/outputDevice` is the same shape. Whether
                    anything is behind it TONIGHT is not: `bound` and `problem`
                    are what the machine found, so they come from the port
                    table and are never stored, exactly as a mount's `loaded`
                    does two branches up. */
                for (const auto& port : container)
                {
                    const auto id = port[idProperty].toString().toStdString();

                    if (id.empty())
                        continue;

                    const auto base = std::string (godot) + "/port/" + id;

                    for (const auto* row : doc::Schema::rowsForOwner ("port"))
                    {
                        const doc::Attribute attribute { "Port", row };
                        const auto name = std::string (row->name);

                        std::string text;

                        if (name == "bound")
                            text = ports != nullptr && ports->isBound (id) ? "true" : "false";
                        else if (name == "problem")
                            text = ports != nullptr ? ports->problemOf (id) : std::string {};
                        else
                            text = storedText (attribute, port);

                        nodes.push_back (makeLeaf (base + "/" + name, *row, text));
                    }
                }
            }
            else if (containerName == "Network")
            {
                /*  THE SHOW'S OWN NETWORK SIDE, which today is one decision:
                    whether a message from a sender nobody declared is obeyed.

                    The STORED half only, like Audio below and for the same
                    reason - how many datagrams have been refused is what the
                    machine has been doing, not what anybody decided, and this
                    half is cached against the document. It is published on the
                    runtime side beside the tick. */
                for (const auto* row : doc::Schema::rowsForOwner ("network"))
                {
                    if (row->persist == doc::Persist::none)
                        continue;

                    const doc::Attribute attribute { "Network", row };

                    nodes.push_back (makeLeaf (std::string (godot) + "/network/"
                                                 + std::string (row->name),
                                               *row, storedText (attribute, container)));
                }
            }
            else if (containerName == "Surfaces")
            {
                /*  THE CONTROL SURFACES THE SHOW DECLARES (PRD §3.16, Phase 6),
                    and every strip on them as a slot of the fourth kind.

                    WHAT A SURFACE IS belongs to the show - its name, its
                    profile, the ports it is on - and WHETHER ANYTHING ANSWERS
                    TONIGHT does not: `connected`, `problem` and `serial` come
                    from the surface table the bridge fills, exactly as a port's
                    `bound` comes from the port table two branches up. A virtual
                    surface is the client's own panel and always connected. */
                for (const auto* row : doc::Schema::rowsForOwner ("surfaces"))
                {
                    const auto name = std::string (row->name);
                    const auto text = name == "order" ? orderOf (container, "Surface") : std::string {};

                    nodes.push_back (makeLeaf (std::string (godot) + "/surface/" + name,
                                               *row, text));
                }

                for (const auto& declared : container)
                {
                    const auto id = declared[idProperty].toString().toStdString();

                    if (id.empty())
                        continue;

                    const auto base = std::string (godot) + "/surface/" + id;
                    const auto profile = declared[juce::Identifier ("profile")].toString().toStdString();
                    const auto isVirtual = profile.empty() || profile == "virtual";
                    const auto status = surfaces != nullptr ? surfaces->statusOf (id)
                                                            : surface::SurfaceTable::Status {};

                    int stripCount = 0;

                    for (const auto& strip : declared)
                        if (strip.getType().toString() == "Strip")
                            ++stripCount;

                    for (const auto* row : doc::Schema::rowsForOwner ("surface"))
                    {
                        const doc::Attribute attribute { "Surface", row };
                        const auto name = std::string (row->name);

                        std::string text;

                        if (name == "strips")
                            text = std::to_string (stripCount);
                        else if (name == "connected")
                            text = isVirtual || status.connected ? "true" : "false";
                        else if (name == "problem")
                            text = isVirtual ? std::string {} : status.problem;
                        else if (name == "serial")
                            text = status.serial;
                        else
                            text = storedText (attribute, declared);

                        nodes.push_back (makeLeaf (base + "/" + name, *row, text));
                    }

                    /*  AND ITS STRIPS, published at /godot/slot/<id> beside the
                        processor inputs and the rack's channels: a run holds
                        one and waits for one the way a Feed holds a slot, so
                        a client reading who holds what reads one table. */
                    int position = 0;

                    for (const auto& strip : declared)
                    {
                        if (strip.getType().toString() != "Strip")
                            continue;

                        const std::map<std::string, std::string> derivedRows {
                            { "surface",  id },
                            { "index",    std::to_string (position++) },
                            { "endpoint", profile == "midiPads" ? "gate" : "absolute" } };

                        collectSlot (strip, "Strip", "strip", "strip", analysis, nodes,
                                     &derivedRows);

                        if (const auto stripId = strip[idProperty].toString().toStdString();
                            ! stripId.empty())
                        {
                            slotOrder.push_back (stripId);

                            const auto stripBase = std::string (godot) + "/slot/" + stripId + "/";
                            stripOrder.push_back ({ stripId,
                                                    document.getAttribute (stripBase + "role")
                                                        .value_or (std::string {}),
                                                    document.getAttribute (stripBase + "dca")
                                                        .value_or (std::string {}) });
                        }
                    }
                }
            }
            else if (containerName == "Dcas")
            {
                /*  THE DCAS (PRD §3.28): a name, a short name, the DCA each sits
                    inside. The TRIM is not here - it is what a fader is doing
                    tonight, not a decision, and this half is a cache rebuilt
                    when the show changes; the runtime half publishes it. */
                for (const auto* row : doc::Schema::rowsForOwner ("dcas"))
                {
                    const auto name = std::string (row->name);
                    const auto text = name == "order" ? orderOf (container, "Dca") : std::string {};

                    nodes.push_back (makeLeaf (std::string (godot) + "/dca/" + name, *row, text));
                }

                for (const auto& dca : container)
                {
                    const auto id = dca[idProperty].toString().toStdString();

                    if (id.empty())
                        continue;

                    const auto base = std::string (godot) + "/dca/" + id;

                    for (const auto* row : doc::Schema::rowsForOwner ("dca"))
                    {
                        const auto name = std::string (row->name);

                        if (name == "trim")
                            continue;

                        const doc::Attribute attribute { "Dca", row };
                        nodes.push_back (makeLeaf (base + "/" + name, *row,
                                                   storedText (attribute, dca)));
                    }

                    dcaOrder.push_back (id);
                }
            }
            else if (containerName == "Audio")
            {
                /*  The one container that publishes attributes of its own -
                    but only the STORED one. `tracks` is what the author
                    decided, so it belongs to this half, which is rebuilt when
                    the document changes.

                    `device`, `outputs` and `status` are what the machine
                    happens to be doing (PRD §4.10), and this half is CACHED:
                    published from here they would be frozen at whatever they
                    were when the document last changed, which for a show that
                    is running and not being edited means for ever. They are
                    emitted on the runtime side instead, beside the tick and
                    the lateness, exactly like /godot/document's runtime half. */
                for (const auto* row : doc::Schema::rowsForOwner ("audio"))
                {
                    if (row->persist == doc::Persist::none)
                        continue;

                    const doc::Attribute attribute { "Audio", row };

                    nodes.push_back (makeLeaf (std::string (godot) + "/audio/"
                                                 + std::string (row->name),
                                               *row, storedText (attribute, container)));
                }

                for (const auto& bus : container)
                {
                    /*  THE RACK IS PASSED OVER HERE by the guard below rather
                        than by a name test: `Rack` is a container element and
                        carries no identifier, exactly like `Mounts`. Had it
                        carried one, `/godot/bus` would have grown a bus with a
                        default width and stopped being the show's buses. */
                    const auto id = bus[idProperty].toString().toStdString();

                    if (id.empty())
                        continue;

                    const auto base = std::string (godot) + "/bus/" + id;

                    for (const auto* row : doc::Schema::rowsForOwner ("bus"))
                    {
                        const doc::Attribute attribute { "Bus", row };
                        const auto name = std::string (row->name);

                        /*  `usage` and `overlaps` ARE FROM HERE, exactly as a
                            slot's are and for the same reason: they change
                            when somebody edits the show and at no other time,
                            so they are a reading of the document like `name`
                            and `width`, and this is the cached half. Empty for
                            a mix channel, because nothing about a mix is a
                            claim. */
                        const auto text = name == "usage"    ? analysis.usageOf (id)
                                        : name == "overlaps" ? analysis.overlapsOf (id)
                                                             : storedText (attribute, bus);

                        nodes.push_back (makeLeaf (base + "/" + name, *row, text));
                    }
                }

                /*  The rack's channels, which are slots of the second kind
                    (§3.9e). The pool is declared here and Phase 9 puts the
                    tracks, the sends and the plugins inside a channel. */
                for (const auto& rack : container)
                {
                    if (rack.getType().toString() != "Rack")
                        continue;

                    for (const auto& channel : rack)
                    {
                        if (channel.getType().toString() != "Channel")
                            continue;

                        collectSlot (channel, "Channel", "rackChannel", "rackChannel",
                                     analysis, nodes);

                        if (const auto channelId = channel[idProperty].toString().toStdString();
                            ! channelId.empty())
                            slotOrder.push_back (channelId);
                    }
                }

                /*  THE PLUGIN SET (Phase 9a, decision AE): the processors this
                    show carries on every voice, in document order - which is
                    the order of the chain. The four rows the machine fills are
                    read off the plugin table and published from this half as a
                    surface's `connected` is: the host that fills the table
                    marks the tree stale when they change. The container's own
                    rows are published whether or not the container exists, so
                    a show with no set reads an empty order rather than nothing. */
                {
                    const auto plugins = container.getChildWithName ("Plugins");

                    for (const auto* row : doc::Schema::rowsForOwner ("plugins"))
                    {
                        const auto name = std::string (row->name);
                        const auto text = name == "order" && plugins.isValid()
                                            ? orderOf (plugins, "Plugin") : std::string {};

                        nodes.push_back (makeLeaf (std::string (godot) + "/plugin/" + name, *row, text));
                    }

                    if (plugins.isValid())
                    {
                        for (const auto& entry : plugins)
                        {
                            if (entry.getType().toString() != "Plugin")
                                continue;

                            const auto id = entry[idProperty].toString().toStdString();

                            if (id.empty())
                                continue;

                            const auto base = std::string (godot) + "/plugin/" + id;
                            const auto status = pluginTable != nullptr ? pluginTable->statusOf (id)
                                                                       : plugin::PluginTable::Status {};

                            /*  THE CATALOGUE, by the entry's identifier: what this
                                machine knows of the plugin's parameters without an
                                instance (§17.7). It answers `paramCount` when the
                                child has not, and every `param/<n>` node below. */
                            const auto identifier = entry[juce::Identifier ("identifier")].toString().toStdString();
                            const auto catalogue = catalogues != nullptr ? catalogues->find (identifier)
                                                                         : std::shared_ptr<const plugin::Catalogue> {};
                            const auto knownCount = catalogue != nullptr ? static_cast<int> (catalogue->params.size()) : 0;

                            for (const auto* row : doc::Schema::rowsForOwner ("plugin"))
                            {
                                const doc::Attribute attribute { "Plugin", row };
                                const auto name = std::string (row->name);

                                std::string text;

                                if (name == "state")               text = status.state;
                                else if (name == "problem")        text = status.problem;
                                else if (name == "latencySamples") text = std::to_string (status.latencySamples);
                                else if (name == "paramCount")     text = std::to_string (status.paramCount > 0 ? status.paramCount : knownCount);
                                else                               text = storedText (attribute, entry);

                                nodes.push_back (makeLeaf (base + "/" + name, *row, text));
                            }

                            /*  EIGHT NODES A PARAMETER, hand-built as the command
                                nodes are because no table row can name them - the
                                list's length is the plugin's. Read-only: a value
                                is written on a cue's Fx, never here. */
                            if (catalogue != nullptr)
                            {
                                const auto leaf = [&nodes] (std::string address, const char* tags,
                                                            std::string description, osc::Value value)
                                {
                                    Node node;
                                    node.address = std::move (address);
                                    node.kind = Kind::state;
                                    node.access = Access::read;
                                    node.typeTags = tags;
                                    node.description = std::move (description);
                                    node.values.push_back (std::move (value));
                                    nodes.push_back (std::move (node));
                                };

                                for (std::size_t n = 0; n < catalogue->params.size(); ++n)
                                {
                                    const auto& parameter = catalogue->params[n];
                                    const auto at = base + "/param/" + std::to_string (n) + "/";
                                    const auto about = "Parameter " + std::to_string (n) + " of " + catalogue->name + ": ";

                                    leaf (at + "name", "s", about + "its name, as the plugin gives it", osc::Value::string (parameter.name));
                                    leaf (at + "shortName", "s", about + "its name for a seven-character display", osc::Value::string (parameter.shortName));
                                    leaf (at + "unit", "s", about + "its unit, as the plugin labels it", osc::Value::string (parameter.unit));
                                    leaf (at + "default", "d", about + "where it rests, normalised 0..1", osc::Value::float64 (static_cast<double> (parameter.defaultValue)));
                                    leaf (at + "min", "d", about + "the least a cue may write, normalised", osc::Value::float64 (0.0));
                                    leaf (at + "max", "d", about + "the most a cue may write, normalised", osc::Value::float64 (1.0));
                                    leaf (at + "steps", "i", about + "how many steps it has, nought when it is continuous", osc::Value::int32 (parameter.discrete ? parameter.steps : 0));
                                    leaf (at + "bipolar", "T", about + "whether its middle is the rest and its ends are opposite - a ring fills from the centre", osc::Value::boolean (parameter.bipolar));
                                }
                            }

                            pluginOrder.push_back (id);
                        }
                    }

                    /*  AND WHAT THIS MACHINE'S SCAN FOUND, beside the set, so a client
                        can offer them: not the show's, and stored nowhere in it. */
                    if (knownPlugins != nullptr)
                    {
                        for (std::size_t n = 0; n < knownPlugins->size(); ++n)
                        {
                            const auto& known = (*knownPlugins)[n];
                            const auto at = std::string (godot) + "/plugin/known/" + std::to_string (n) + "/";

                            const auto leaf = [&nodes, &at] (const char* name, const char* description, const std::string& value)
                            {
                                Node node;
                                node.address = at + name;
                                node.kind = Kind::state;
                                node.access = Access::read;
                                node.typeTags = "s";
                                node.description = description;
                                node.values.push_back (osc::Value::string (value));
                                nodes.push_back (std::move (node));
                            };

                            leaf ("name", "A plugin this machine has, by the name its file gives", known.name);
                            leaf ("identifier", "The identifier plugin.create takes for it", known.identifier);
                            leaf ("format", "Its format: VST3, AudioUnit", known.format);
                            leaf ("manufacturer", "Who made it, as the file says", known.manufacturer);
                        }
                    }
                }
            }
        }

        //----------------------------------------------------------------------
        /*  `/godot/slot/order`, the container's own node, beside
            `/godot/list/order` and `/godot/run/order` and built on the same
            machinery §12.12 put in for them. */
        {
            std::string joined;

            for (const auto& id : slotOrder)
            {
                if (! joined.empty())
                    joined += ' ';

                joined += id;
            }

            for (const auto* row : doc::Schema::rowsForOwner ("slots"))
                nodes.push_back (makeLeaf (std::string (godot) + "/slot/"
                                             + std::string (row->name),
                                           *row, joined));

            /*  KEPT FOR THE RUNTIME HALF, which has no document to walk. Who
                holds a slot changes several times a second while nothing about
                the show does, so `holder` and `pending` cannot be published
                from this cached half - they would freeze at whatever they were
                when somebody last edited a cue, which for a show that is
                running and not being edited means for ever. Same reason
                `/godot/audio/status` is not published here either. */
            declaredSlots = slotOrder;
        }

        declaredCues = std::move (cueOrder);
        declaredMedia = std::move (mediaOrder);
        declaredLists = std::move (listOrder);
        declaredDcas = std::move (dcaOrder);
        declaredPlugins = std::move (pluginOrder);
        declaredStrips = std::move (stripOrder);

        //----------------------------------------------------------------------
        /*  Commands, as write-only method nodes. `node.set` is deliberately
            absent: its signature is whatever the target node declares, so it
            has no fixed one to publish, and its address IS the target's. */
        for (const auto& command : commands.all())
        {
            if (command.name == "node.set")
                continue;

            auto address = std::string (godot) + "/cmd/" + command.name;
            std::replace (address.begin(), address.end(), '.', '/');

            Node node;
            node.address = std::move (address);
            node.kind = Kind::event;
            node.access = Access::write;
            node.description = command.description;

            for (const auto& param : command.params)
                node.typeTags += param.typeTag;

            nodes.push_back (std::move (node));
        }

        /*  EVERY INSERT'S PARAMETER NODES (Phase 9a, PR 9a.8): for each Fx of
            each media cue, `p<n>` - a value a hand writes, one node a
            parameter, through the FX door - and `t<n>`, the plugin's own
            text for it, off the catalogue's table with no round trip. Only
            when the catalogue knows the plugin: with nothing known there is
            nothing to name, and the row `values` still holds what was written.
            A value a cue stores rests at the catalogue's default when the cue
            does not set it. */
        {
            std::function<void (const juce::ValueTree&)> walk = [&] (const juce::ValueTree& element)
            {
                if (element.hasType ("Fx"))
                {
                    const auto fxId = element[idProperty].toString().toStdString();
                    const auto entry = setEntryFor (element, element["plugin"].toString().toStdString());

                    if (fxId.empty() || ! entry.element.isValid() || catalogues == nullptr)
                        return;

                    const auto catalogue = catalogues->find (entry.element["identifier"].toString().toStdString());

                    if (catalogue == nullptr)
                        return;

                    const auto stored = cue::parseFxValues (element["values"].toString().toStdString());
                    const auto base = std::string (godot) + "/fx/" + fxId + "/";

                    for (std::size_t n = 0; n < catalogue->params.size(); ++n)
                    {
                        const auto& parameter = catalogue->params[n];
                        const auto found = stored.find (static_cast<int> (n));
                        const auto value = found != stored.end() ? static_cast<float> (found->second)
                                                                 : parameter.defaultValue;

                        Node p;
                        p.address = base + "p" + std::to_string (n);
                        p.kind = Kind::state;
                        p.access = Access::readWrite;
                        p.typeTags = "d";
                        p.description = parameter.name.empty() ? "Parameter " + std::to_string (n) : parameter.name;
                        p.unit = parameter.unit;
                        p.hasMinimum = true;
                        p.minimum = 0.0;
                        p.hasMaximum = true;
                        p.maximum = 1.0;

                        if (parameter.discrete)
                            p.enumValues = parameter.stepText;

                        p.values.push_back (osc::Value::float64 (static_cast<double> (value)));
                        nodes.push_back (std::move (p));

                        Node t;
                        t.address = base + "t" + std::to_string (n);
                        t.kind = Kind::state;
                        t.access = Access::read;
                        t.typeTags = "s";
                        t.description = "What the plugin calls the value of p" + std::to_string (n) + ", in its own words";
                        t.values.push_back (osc::Value::string (parameter.textFor (value)));
                        nodes.push_back (std::move (t));
                    }

                    /*  A stored index past the count is published as it is: a
                        show written against another version of the plugin keeps
                        what it wrote, and says so by having a node the catalogue
                        cannot name. */
                    for (const auto& [index, value] : stored)
                    {
                        if (index < static_cast<int> (catalogue->params.size()))
                            continue;

                        Node p;
                        p.address = base + "p" + std::to_string (index);
                        p.kind = Kind::state;
                        p.access = Access::readWrite;
                        p.typeTags = "d";
                        p.description = "A value written against a version of the plugin with more parameters than this one";
                        p.hasMinimum = true;
                        p.hasMaximum = true;
                        p.maximum = 1.0;
                        p.values.push_back (osc::Value::float64 (value));
                        nodes.push_back (std::move (p));
                    }

                    return;
                }

                for (const auto child : element)
                    walk (child);
            };

            walk (showNode.getChildWithName ("Lists"));
        }

        addContainers (nodes, {}, true);
        sortByAddress (nodes);

        documentPart = std::make_shared<const std::vector<Node>> (std::move (nodes));
        pluginRevision = pluginTable != nullptr ? pluginTable->revision() : 0;
        catalogueRevision = catalogues != nullptr ? catalogues->revision() : 0;
        stale = false;
    }

    /*  SOMEBODY ELSE'S NAMESPACE, at its own prefix and on its own clock.

        It used to be part of the document half, on the argument that it changes
        only when a mount is loaded or written to - both of which mark the tree
        stale. True, and beside the point: the document half is ALSO rebuilt by
        everything else, and a mounted WFS-DIY capture is 2487 nodes to a show's
        150.

        M9 measured what that cost. One applied mutation with the capture
        mounted: 3.24 ms in Release, of which 3.13 ms was this - twenty-nine
        times the rest of the tree put together, and 16% of a tick, paid every
        time anybody renamed a cue. In Debug it is 78 ms, which is four ticks.

        So it is its own cache now, rebuilt when the mount table says it has
        moved and not otherwise. */
    void ParameterTree::rebuildMountPart()
    {
        auto nodes = mounts.allNodes();

        /*  THE CONTAINERS ALONG EACH MOUNT'S PREFIX come with it, because they
            describe a path that exists only because the mount does: /wfs and
            /wfs/input are nobody's nodes until somebody mounts a processor
            there. Built here rather than in the document half so that
            unloading a mount takes its path with it. */
        addContainers (nodes, {}, false);
        sortByAddress (nodes);

        mountPart = std::make_shared<const std::vector<Node>> (std::move (nodes));
        mountRevision = mounts.revision();
        ++mountRebuildCount;
    }

    namespace
    {
        /** Identifiers as a container's `order` spells them: space-separated. */
        std::string joinIds (const std::vector<std::string>& ids)
        {
            std::string out;

            for (const auto& id : ids)
            {
                if (! out.empty())
                    out += ' ';

                out += id;
            }

            return out;
        }

        /*  `/godot/run/<id>/timbre`: the frame playing at the run's position,
            as three numbers, or nothing - and nothing is "not analysed yet",
            never a colour (§14.5).

            ROUNDED BEFORE IT IS PRINTED: the hue to a tenth of a degree, the
            saturation and the lightness to a thousandth. A frame is four bytes,
            so that is fine enough that every byte still prints differently from
            its neighbours - a hue step is 1.40625 degrees and a unit step
            1/255 - and coarse enough that 52/255 does not reach a client as
            seventeen digits of a byte's worth of information. Printed by the
            formatter the whole tree uses, so a French locale moves no decimal
            point.

            EMPTY for a run that plays no file, for a tree that was handed no
            MediaInfo - a test's - and for a file whose pyramid
            has not arrived. A silent frame is `0 0 0`, and that is a reading:
            lightness nought is below the ramp's darkest, so no client can take
            it for a low sound. */
        /*  HOW MUCH OF THE WAIT IT IS IN IS LEFT, in seconds, and nought
            when it is not in one. `dueTick` is meaningful only while the state
            is `waiting` or `postWait` - Run.h says so - so the state is asked
            first and the deadline second. */
        double remainingOf (const cue::Run& run, std::int64_t tick)
        {
            if (run.state != cue::runState::waiting && run.state != cue::runState::postWait)
                return 0.0;

            const auto left = run.dueTick - tick;

            return left > 0 ? static_cast<double> (left)
                                / static_cast<double> (TickClock::rateHz)
                            : 0.0;
        }

        std::string timbreText (const cue::Run& run, const audio::MediaRecords* records)
        {
            if (records == nullptr || run.media.empty())
                return {};

            const auto found = records->find (run.media);

            if (found == records->end() || found->second.pyramid == nullptr)
                return {};

            const auto* frame = audio::timbre::frameAt (*found->second.pyramid, run.position);

            if (frame == nullptr)
                return {};

            const auto rounded = [] (double value, double steps)
            {
                return osc::formatDouble (std::round (value * steps) / steps);
            };

            return rounded (audio::timbre::hueOf (*frame), 10.0) + " "
                 + rounded (audio::timbre::saturationOf (*frame), 1000.0) + " "
                 + rounded (audio::timbre::lightnessOf (*frame), 1000.0);
        }
    }

    //==============================================================================
    std::shared_ptr<const TreeSnapshot> ParameterTree::publish (std::int64_t tick,
                                                                const EngineState& state)
    {
        /*  ASKED, AND ASKED FIRST. Both halves below read it - the document
            half for a slot's `usage` and `overlaps`, the runtime half for
            `/godot/document/warnings` - and it answers out of a cache keyed on
            the document's own revision, so a tick that changed nothing costs a
            comparison of two integers. M18 counts the rebuilds. */
        /*  THE LENGTHS FOR THIS PASS, taken before anything reads them and
            held until the next. One is LEARNED when the analyser reads a file
            imported since the show opened, and learning swaps the map - so the
            address moves, the slot analysis below sees a new one and rebuilds
            exactly once, and a cue whose file arrived this session stops
            reading a length of nought. */
        if (fixedDurations != nullptr)
        {
            durations = fixedDurations;
        }
        else if (mediaInfo != nullptr)
        {
            durationsHeld = mediaInfo->durations();
            durations = durationsHeld.get();
        }

        analysis.ensureBuilt (document, durations);

        /*  AND THE PLUGIN TABLE ASKED THE SAME WAY (Phase 9a): the sandbox
            writes it from the message thread, and its `state` rows are on
            this cached half. */
        if (stale || documentPart == nullptr
             || (pluginTable != nullptr && pluginTable->revision() != pluginRevision)
             || (catalogues != nullptr && catalogues->revision() != catalogueRevision))
            rebuildDocumentPart();

        /*  ASKED RATHER THAN TOLD. The mount table bumps its own revision on
            every load, unload, write and read-back, so this cannot be left
            behind by a mount path somebody adds later - which a second
            `markStale` would have been the first time one was. */
        if (mountPart == nullptr || mounts.revision() != mountRevision)
            rebuildMountPart();

        std::vector<Node> runtime;

        /*  The engine's own numbers, and the three that say which bundle is
            open. Both are runtime: PRD §4.10 keeps "what the machine happened
            to be doing" out of the document, and the tree is where it surfaces
            instead. */
        const auto engineValue = [&runtime] (const doc::AttributeRow& row,
                                             std::string_view owner,
                                             const std::string& text)
        {
            runtime.push_back (makeLeaf (std::string (godot) + "/" + std::string (owner) + "/"
                                           + std::string (row.name),
                                         row, text));
        };

        for (const auto* row : doc::Schema::rowsForOwner ("engine"))
        {
            const auto name = std::string (row->name);
            std::string text;

            if (name == "product")             text = state.product;
            else if (name == "version")        text = state.version;
            else if (name == "tick")           text = std::to_string (state.tick);
            else if (name == "sampleRate")     text = std::to_string (state.sampleRate);
            else if (name == "blockSize")      text = std::to_string (state.blockSize);
            else if (name == "samplesPerTick") text = std::to_string (state.samplesPerTick);
            else if (name == "lateness")       text = std::to_string (state.lateness);
            else if (name == "latenessMax")    text = std::to_string (state.latenessMax);
            else if (name == "clock")          text = state.clock;
            else if (name == "errorCount")     text = std::to_string (state.errorCount);
            else if (name == "launchLatencyTicks")
                                               text = std::to_string (state.launchLatencyTicks);
            /*  TWO TICKS PLUS THE LAUNCH LATENCY, and the two are the price of
                the scheduler deciding by SUBMITTING rather than by acting: a
                member's `run.ended` is applied during one tick, the scheduler
                sees it on the next and asks for the next member, and the launch
                goes in on the one after. That is what lets a whole show replay,
                and it is published rather than left for somebody to find with a
                stopwatch. Zero when there is no audio side, because there is
                then no launch to be late for. */
            else if (name == "sequenceGapTicks")
                                               text = std::to_string (state.launchLatencyTicks > 0
                                                                        ? state.launchLatencyTicks + 2
                                                                        : 0);
            else if (name == "rtViolations")   text = std::to_string (state.rtViolations);
            else if (name == "rtForeignAllocations")
                                               text = std::to_string (state.rtForeignAllocations);
            else if (name == "lastError")      text = state.lastError;
            else if (name == "analysisRebuilds")
                                               text = std::to_string (analysis.rebuilds());
            else                               text = std::string (row->defaultText);

            engineValue (*row, "engine", text);
        }

        for (const auto* row : doc::Schema::rowsForOwner ("document"))
        {
            if (row->persist != doc::Persist::none)
                continue;

            const auto name = std::string (row->name);
            std::string text;

            if (name == "path")       text = state.documentPath;
            else if (name == "name")  text = state.documentName;
            else if (name == "dirty") text = state.documentDirty ? "true" : "false";

            /*  The show half's change count, as a number a client keys a
                cached picture on - EngineState says what moves it and, more
                to the point, what does not. */
            else if (name == "revision") text = std::to_string (state.documentRevision);

            /*  The undo history, as four readings and not one: what a client
                needs to draw a menu is whether each half is available and what
                each would be called, and a single string carrying all of it
                would be a format somebody has to parse. */
            else if (name == "canUndo")  text = state.documentCanUndo ? "true" : "false";
            else if (name == "canRedo")  text = state.documentCanRedo ? "true" : "false";
            else if (name == "undoName") text = state.documentUndoName;
            else if (name == "redoName") text = state.documentRedoName;
            else if (name == "undoHistory") text = state.documentUndoHistory;
            else if (name == "redoHistory") text = state.documentRedoHistory;

            //  What was last copied, for a client to carry to its clipboard or paste back.
            else if (name == "clipboard") text = state.documentClipboard;

            /*  Whether a previous session left work in `recovery/`. It changes
                at most twice in a session - once when the bundle opens and once
                when the operator answers it - which is why the table caps it at
                1 rather than at the 5 its livelier neighbours carry. */
            else if (name == "recovery") text = state.documentRecovery ? "true" : "false";
            else if (name == "recording") text = lists != nullptr && lists->isRecording() ? "true" : "false";

            /*  And why the last write handed to the writer thread did not land,
                beside it: a sentence, because it is the writer's own - which
                file, and what became of it - and empty when nothing is
                outstanding, so a client shows it by showing it when it is not
                empty. */
            else if (name == "writeError") text = state.documentWriteError;

            /*  FROM THE RUNTIME HALF although it is a reading of the document,
                because it must never be stale: a client asking what is wrong
                with the show is asking about the show as it is now, and the
                cached half rebuilds only when somebody remembered to say so.
                The answer itself costs nothing here - the walk behind it is the
                analysis cache, which the top of `publish` already asked. */
            else if (name == "warnings") text = analysis.warningText();
            else                      text = std::string (row->defaultText);

            engineValue (*row, "document", text);
        }

        /*  What the audio actually is. Absent a driver these are the table's
            own defaults - no device, no outputs, stopped - and that is the
            truthful reading of a process that has not opened one, not a
            placeholder standing in for a number nobody took. */
        for (const auto* row : doc::Schema::rowsForOwner ("audio"))
        {
            if (row->persist != doc::Persist::none)
                continue;

            const auto name = std::string (row->name);
            std::string text;

            if (name == "device")        text = state.audioDevice;
            else if (name == "outputs")  text = std::to_string (state.audioOutputs);
            else if (name == "status")   text = state.audioStatus;
            else if (name == "settingsStatus") text = state.audioSettingsStatus;
            else if (name == "settingsRevision") text = std::to_string (state.audioSettingsRevision);
            else if (name == "settingsError") text = state.audioSettingsError;
            else if (name == "actualSampleRate") text = std::to_string (state.audioSampleRate);
            else if (name == "actualBufferSize") text = std::to_string (state.audioBufferSize);
            else if (name == "availableBufferSizes") text = state.audioAvailableBufferSizes;
            else if (name == "testType") text = std::to_string (state.audioTest.type);
            else if (name == "testChannel") text = std::to_string (state.audioTest.channel);
            else if (name == "testFrequency") text = std::to_string (state.audioTest.frequency);
            else if (name == "testLevel") text = juce::String (state.audioTest.level, 1).toStdString();
            else if (name == "testHold") text = state.audioTest.hold ? "true" : "false";
            else if (name == "hardwareInputs") text = std::to_string (state.hardwareInputs);
            else if (name == "hardwareOutputs") text = std::to_string (state.hardwareOutputs);
            else                         text = std::string (row->defaultText);

            engineValue (*row, "audio", text);
        }

        /*  WHAT THIS MACHINE HAS TO PLUG A PORT INTO.

            Here rather than in the cached half because it changes when the
            document does not: somebody plugs an interface in during a tech
            rehearsal and asks for a rescan, and the menus have to grow the new
            name without the show being edited. */
        for (const auto* row : doc::Schema::rowsForOwner ("ports"))
        {
            if (row->persist != doc::Persist::none)
                continue;

            const auto name = std::string (row->name);
            std::string text;

            if (name == "inputs" && ports != nullptr)
                text = midi::PortTable::namesOf (ports->inputs());
            else if (name == "outputs" && ports != nullptr)
                text = midi::PortTable::namesOf (ports->outputs());
            else
                text = std::string (row->defaultText);

            engineValue (*row, "port", text);
        }

        /*  AND WHAT THE NETWORK SIDE HAS BEEN DOING. One number today: how
            many datagrams the sender gate has dropped since the show opened.

            Here rather than in the cached half because it climbs while the
            document sits still - which is the whole use of it. Somebody whose
            surface has gone quiet presses a button and watches: a number that
            moves says the message is arriving and being refused, which is a
            different evening from one that never arrives at all. */
        for (const auto* row : doc::Schema::rowsForOwner ("network"))
        {
            if (row->persist != doc::Persist::none)
                continue;

            const auto name = std::string (row->name);
            std::string text;

            if (name == "refused") text = std::to_string (state.refusedDatagrams);
            else                   text = std::string (row->defaultText);

            engineValue (*row, "network", text);
        }

        /*  EVERY RUN, EVERY TICK. A run changes several times a second while
            nothing about the show does, which is exactly why it is here and not
            in the cached half: published from there it would have been frozen
            at whatever it read the last time somebody edited a cue.

            A finished run keeps its address for `cue::retentionTicks` and is
            then dropped - five seconds, which is long enough for a client
            polling at the tick rate to see the `done` it was waiting for and
            for a person to read it. Gogo is the pure present tense (§7), and a
            four-hour show would otherwise publish four hours of finished runs
            on every one of its 720 000 ticks.

            IT RETIRES FROM THE TREE AND NOT FROM THE TABLE, and the difference
            is the whole design of it. Erasing the run would make the MODEL
            depend on something only a live session does: hooks do not run
            during a replay, so a `run.kill` arriving six seconds after its run
            ended would be rejected live (the run is gone) and applied on replay
            (it is not), and the session would fail to reproduce itself.
            Nothing else can tell the difference - `liveRunOf` and
            `lowestFreeTrack` both skip finished runs already - so what is left
            is a tree that stops growing, which was the only real problem.

            The clock it reads is the run's OWN ending tick, written by the
            handler that finished it, so what is published at tick N is the same
            set live and replayed. */
        /*  THE ROSTER, so a client can ask what is running without walking the
            tree and guessing which addresses appeared since last time. It lists
            exactly what is published below, retention included - a run that has
            stopped being published has stopped being in the order. */
        std::string runOrder;

        for (const auto& run : runs.all())
        {
            if (run.id.empty())
                continue;

            if (run.endedAtTick >= 0 && tick - run.endedAtTick > cue::retentionTicks)
                continue;

            if (! runOrder.empty())
                runOrder += ' ';

            runOrder += run.id;
        }

        /*  WHERE EACH LIST IS POINTED, AND WHAT THE SHOW WOULD BE THERE.

            All three out of the runtime half, because none of them is about the
            show: an aim is where somebody's finger is, a solve is an answer to
            that question, and a state position is where a jump landed. §4.10
            keeps every one of them out of the document, and the cached half
            would freeze all three at whatever they were when a cue was last
            edited.

            THE SOLVE IS COMPUTED WHEN THE QUESTION CHANGES, not on every
            publish: it is a walk of a list, its rate cap is five hertz, and the
            question is the aim plus the document's revision - the same aim over
            an edited show being a different answer. */
        for (const auto& listId : declaredLists)
        {
            const auto base = std::string (godot) + "/list/" + listId;
            const auto aim = lists != nullptr ? lists->aimOf (listId) : cue::ListAim {};

            if (const auto* row = rowNamed ("list", "aim"))
                runtime.push_back (makeLeaf (base + "/aim", *row, cue::spellAim (aim)));

            if (const auto* row = rowNamed ("list", "statePosition"))
                runtime.push_back (makeLeaf (base + "/statePosition", *row,
                                             cue::spellAim (lists != nullptr
                                                              ? lists->positionOf (listId)
                                                              : cue::ListAim {})));

            /*  THE STEPS, NEWEST FIRST, because the use is going back a little
                and the first thing an operator reads should be the last thing
                that happened. Kept newest-last in the state, where appending is
                cheap, and turned round here, where it is read. */
            if (const auto* row = rowNamed ("list", "history"))
            {
                std::string text;

                if (lists != nullptr)
                {
                    const auto& steps = lists->historyOf (listId);

                    for (auto step = steps.rbegin(); step != steps.rend(); ++step)
                    {
                        if (! text.empty())
                            text += ' ';

                        text += cue::spellStep (*step);
                    }
                }

                runtime.push_back (makeLeaf (base + "/history", *row, text));
            }

            if (const auto* row = rowNamed ("list", "solve"))
            {
                /*  THE HISTORY IS PART OF THE QUESTION (2026-09-19): the
                    answer is read from the steps when the aimed cue has one,
                    so a GO, or a jump moving the steps, is a new question. */
                std::string history;

                if (lists != nullptr)
                    for (const auto& step : lists->historyOf (listId))
                        history += cue::spellStep (step) + " ";

                const auto question = std::to_string (document.revision()) + " "
                                        + cue::spellAim (aim) + " " + history;

                if (solvedFor[listId] != question)
                {
                    solvedFor[listId] = question;

                    solves[listId] = aim.isSet()
                                       ? cue::solveAim (document, durations, &mounts,
                                                        { listId, aim.cue, aim.offset },
                                                        lists != nullptr ? &lists->historyOf (listId)
                                                                         : nullptr).toJson()
                                       : std::string {};
                }

                runtime.push_back (makeLeaf (base + "/solve", *row, solves[listId]));
            }
        }

        /*  HOW FAR AHEAD EACH CUE HAS BEEN GOT READY.

            One node per cue, every tick, out of the half that is never stale.
            Built from a map rather than by asking the run table per cue,
            because that would be a scan of every run for every row in the show:
            what carries the answer is `Run::prepare`, and only a run that is
            preparing something has one.

            §13.6's `idle` is the empty string here and is the resting state
            rather than a failure - a MIDI cue can never be prepared and reads
            it for ever. */
        {
            std::map<std::string, std::string> preparedness;

            for (const auto& run : runs.all())
                if (! run.prepare.empty())
                    preparedness[run.cue] = run.prepare;

            const auto* row = rowNamed ("cue", "prepare");

            if (row != nullptr)
                for (const auto& cueId : declaredCues)
                {
                    const auto found = preparedness.find (cueId);

                    runtime.push_back (makeLeaf (std::string (godot) + "/cue/" + cueId
                                                   + "/prepare",
                                                 *row,
                                                 found != preparedness.end()
                                                   ? found->second
                                                   : std::string (row->defaultText)));
                }
        }

        /*  WHAT THE ANALYSER HAS PUBLISHED, ASKED ONCE. Every hash below and
            every run's timbre further down read this one map, so a publish
            costs one pointer copy under MediaInfo's short lock however many
            clips are playing - and all of them answer out of the same moment,
            rather than a record landing between two runs and giving one of
            them colours the other does not have. Empty with no MediaInfo. */
        const auto mediaRecords = mediaInfo != nullptr ? mediaInfo->snapshot()
                                                       : std::shared_ptr<const audio::MediaRecords> {};

        /*  THE HASH OF EACH MEDIA CUE'S FILE, against the roster the document
            half left behind: `prepare`'s pair, the other half of the skip in
            `collectCue` (§14.5). A hash arrives from the analyser's thread
            while nothing about the show moves, so it is read here, where
            nothing is cached.

            EMPTY UNTIL THE RECORD HAS A PYRAMID AS WELL AS A HASH. The analyser
            publishes the two together, and asking for both here too means no
            client can ever read a hash that `/media/<hash>/timbre` would answer
            with a 404. */
        if (const auto* row = rowNamed ("media", "hash"))
            for (const auto& [cueId, file] : declaredMedia)
            {
                std::string text;

                if (mediaRecords != nullptr)
                    if (const auto found = mediaRecords->find (file);
                        found != mediaRecords->end() && found->second.pyramid != nullptr)
                        text = found->second.contentHash;

                runtime.push_back (makeLeaf (std::string (godot) + "/cue/" + cueId + "/hash",
                                             *row, text));
            }

        /*  WHO HOLDS EACH DECLARED SLOT, AND WHO IS WAITING FOR IT.

            Read off the run table rather than kept beside it, which is the
            answer `isTrackBusy` already gives for a voice: a second record of
            who holds what is a second thing to keep in step, and the one that
            is wrong is always the copy. So this is a scan, and it is the same
            scan on a replay.

            The roster comes from the document half, which walked the show the
            last time it changed. */
        for (const auto& slotId : declaredSlots)
        {
            const auto base = std::string (godot) + "/slot/" + slotId;

            for (const auto* row : doc::Schema::rowsForOwner ("slot"))
            {
                const auto name = std::string (row->name);

                if (name != "holder" && name != "pending")
                    continue;

                std::string text;

                if (name == "holder")
                {
                    const auto* holder = runs.holderOf (slotId);
                    text = holder != nullptr ? holder->id : std::string {};
                }
                else
                {
                    std::vector<std::string> waiting;

                    for (const auto* run : runs.waitersFor (slotId))
                        waiting.push_back (run->id);

                    text = joinIds (waiting);
                }

                runtime.push_back (makeLeaf (base + "/" + name, *row, text));
            }
        }

        for (const auto* row : doc::Schema::rowsForOwner ("runs"))
            runtime.push_back (makeLeaf (std::string (godot) + "/run/" + std::string (row->name),
                                         *row, runOrder));

        /*  WHAT EACH STRIP IS DOING (PRD §3.16, §3.27), read off the run table
            the way a slot's holder is: the node its fader rides now, the word
            its display shows, and the cue on it. A dca strip rides its DCA's
            trim; a sampler strip rides the trim of the run holding it, which
            changes at every handover - that is how one fader plays a different
            sound after a bank change. */
        for (const auto& strip : declaredStrips)
        {
            std::string target;
            std::string word;
            std::string cueText;

            if (strip.role == "dca")
            {
                word = strip.dca.empty() ? "unassigned" : "dca";

                if (! strip.dca.empty())
                    target = std::string (godot) + "/dca/" + strip.dca + "/trim";
            }
            else if (const auto* holder = runs.holderOf (strip.id); holder != nullptr)
            {
                target = std::string (godot) + "/run/" + holder->id + "/trim";
                cueText = holder->cue;

                const auto* group = runs.find (holder->parent);
                const auto closing = group != nullptr
                                       && (group->closing
                                             || std::find (group->lostStrips.begin(),
                                                           group->lostStrips.end(), strip.id)
                                                  != group->lostStrips.end());

                if (closing)
                    word = "closing";
                else if (holder->held)
                    word = "held";
                else if (holder->state == cue::runState::stopping)
                    word = "stopping";
                else if (holder->state == cue::runState::playing
                          || holder->state == cue::runState::waiting
                          || holder->launchRequested)
                    word = "playing";
                else if (! holder->pending.empty())
                    word = "pending";
                else
                    word = "armed";
            }
            else if (const auto waiting = runs.waitersFor (strip.id); ! waiting.empty())
            {
                word = "pending";
                cueText = waiting.front()->cue;
            }
            else
            {
                word = "free";
            }

            const auto base = std::string (godot) + "/slot/" + strip.id + "/";

            for (const auto* row : doc::Schema::rowsForOwner ("strip"))
            {
                const auto name = std::string (row->name);

                if (name == "target")
                    runtime.push_back (makeLeaf (base + name, *row, target));
                else if (name == "word")
                    runtime.push_back (makeLeaf (base + name, *row, word));
                else if (name == "cue")
                    runtime.push_back (makeLeaf (base + name, *row, cueText));
            }
        }

        /*  WHAT EACH DCA IS TRIMMING BY TONIGHT (PRD §3.28), against the
            roster the document half left behind. From this half because a trim
            is what a fader is doing - it moves fifty times a second while a
            hand rides it and nothing about the show does - and the other half
            is a cache. */
        for (const auto& dcaId : declaredDcas)
            for (const auto* row : doc::Schema::rowsForOwner ("dca"))
                if (row->name == "trim")
                    runtime.push_back (makeLeaf (std::string (godot) + "/dca/" + dcaId + "/trim", *row,
                                                 osc::formatDouble (dcas != nullptr ? dcas->trimOf (dcaId)
                                                                                    : 0.0)));

        for (const auto& run : runs.all())
        {
            if (run.id.empty())
                continue;

            if (run.endedAtTick >= 0 && tick - run.endedAtTick > cue::retentionTicks)
                continue;

            const auto base = std::string (godot) + "/run/" + run.id;

            for (const auto* row : doc::Schema::rowsForOwner ("run"))
            {
                const auto name = std::string (row->name);
                std::string text;

                if (name == "cue")            text = run.cue;
                else if (name == "kind")      text = run.kind;
                else if (name == "state")     text = run.state;
                else if (name == "track")     text = std::to_string (run.track);
                else if (name == "position")  text = osc::formatDouble (run.position);

                /*  WHEN IT STARTED, as the tick a GO was applied on - which is
                    what the runner already keeps in order to measure lateness,
                    handed out so a client can order a pane by it. */
                else if (name == "started")   text = std::to_string (run.launchRequestedAtTick);

                /*  HOW LONG UNTIL IT DOES THE NEXT THING, and nought whenever
                    it is not in a wait - so a reader never has to ask the state
                    node whether this one means anything.

                    FROM THE DEADLINE AND NOT FROM A COUNTER. `dueTick` is an
                    absolute tick a handler set from its own tick, so this is a
                    subtraction rather than an accumulation: a client that
                    missed twenty publishes reads the truth on the next one, and
                    nothing here can drift. */
                else if (name == "remaining") text = osc::formatDouble (remainingOf (run, tick));

                /*  A table lookup at that same position, every tick: the 10 in
                    the row is what a surface is told to draw it at, and nothing
                    here throttles it (§14.5). */
                else if (name == "timbre")    text = timbreText (run, mediaRecords.get());
                else if (name == "level")     text = osc::formatDouble (run.level);
                else if (name == "trim")      text = osc::formatDouble (run.trim);
                else if (name == "late")      text = std::to_string (run.late);
                else if (name == "parent")    text = run.parent;
                else if (name == "children")  text = joinIds (run.children);
                else if (name == "phase")     text = run.phase;
                else if (name == "claims")    text = joinIds (run.claims);
                else if (name == "pending")   text = joinIds (run.pending);
                else if (name == "warning")   text = run.warning;
                else if (name == "asserted")  text = run.asserted ? "true" : "false";
                else if (name == "strip")     text = run.strip;
                else if (name == "held")      text = run.held ? "true" : "false";
                else if (name == "error")     text = run.error;
                else if (name == "iteration")  text = std::to_string (run.iteration);
                else if (name == "iterations") text = std::to_string (run.iterations);
                else if (name == "round")      text = joinIds (run.round);
                else if (name == "pruned")     text = joinIds (run.pruned);
                else if (name == "seed")       text = std::to_string (run.seed);
                else if (name == "range")      text = std::to_string (run.range);
                else if (name == "rangeIteration")
                                               text = std::to_string (run.rangeIteration);
                else                          text = std::string (row->defaultText);

                runtime.push_back (makeLeaf (base + "/" + name, *row, text));
            }
        }

        /*  "/", "/godot" and "/godot/document" belong to the document half, so
            the runtime half must not carry them too - `find` looks in one and
            then the other, and a duplicate would make the answer depend on
            which it reached first. What is left for this side to add is
            "/godot/engine".

            AND EVERY CUE AND EVERY SLOT, which is new here and closes a hole
            that was already open: the runtime half has published a slot's
            `holder` and `pending` since PR 4.3, so `/godot/slot` and
            `/godot/slot/<id>` were already being carried by both halves. No
            test caught it because the case needs a show with a declared slot AND
            the whole-tree walk that counts addresses, and no fixture had both.
            `prepare` puts every CUE in the same position, which is what made it
            visible. A media cue's `hash` needs nothing more: a media cue is a
            cue, so `declaredCues` already names its container, and
            `/godot/run/<id>`, where `timbre` lives, is this half's alone. */
        std::vector<std::string> ownedByTheDocument {
            std::string (rootAddress), std::string (godot),
            std::string (godot) + "/document",
            std::string (godot) + "/audio",

            /*  `/godot/network` belongs to the document half for the reason
                `/godot/audio` does: both halves publish rows under it -
                `strictSenders` is a decision and `refused` is a count - and
                whichever one minted the container would decide what `find`
                answered for it. The show's half owns it, since a show always
                has a Network element and may have no engine running. */
            std::string (godot) + "/network",

            /*  `/godot/port` for the reason `/godot/audio` is here: both
                halves publish under it - the declared ports from the show,
                this machine's device lists from the runtime - and whichever
                one minted the container would decide what `find` answered. */
            std::string (godot) + "/port",
            std::string (godot) + "/cue",
            std::string (godot) + "/list",
            std::string (godot) + "/slot",

            /*  `/godot/dca` for the reason `/godot/port` is here: both halves
                publish under it - a DCA's name from the show, its trim from
                the fader - and whichever minted the container would decide
                what `find` answered for it. */
            std::string (godot) + "/dca",

            /*  `/godot/plugin` for the same reason: the set's rows from the
                show, and later the catalogue and the known list beside them. */
            std::string (godot) + "/plugin" };

        for (const auto& id : declaredDcas)
            ownedByTheDocument.push_back (std::string (godot) + "/dca/" + id);

        for (const auto& id : declaredPlugins)
            ownedByTheDocument.push_back (std::string (godot) + "/plugin/" + id);

        for (const auto& id : declaredLists)
            ownedByTheDocument.push_back (std::string (godot) + "/list/" + id);

        for (const auto& id : declaredCues)
            ownedByTheDocument.push_back (std::string (godot) + "/cue/" + id);

        for (const auto& id : declaredSlots)
            ownedByTheDocument.push_back (std::string (godot) + "/slot/" + id);

        addContainers (runtime, ownedByTheDocument, false);
        sortByAddress (runtime);

        auto result = std::make_shared<const TreeSnapshot> (tick, documentPart, mountPart,
                                                            std::move (runtime));

        {
            const std::lock_guard<std::mutex> lock { publishMutex };
            published = result;
        }

        return result;
    }

    std::shared_ptr<const TreeSnapshot> ParameterTree::snapshot() const
    {
        {
            const std::lock_guard<std::mutex> lock { publishMutex };

            if (published != nullptr)
                return published;
        }

        /*  Never nullptr, so no caller has to check. An empty tree at tick -1
            is the honest description of an engine that has not ticked yet. */
        return std::make_shared<const TreeSnapshot> (
            -1, std::make_shared<const std::vector<Node>>(),
            std::make_shared<const std::vector<Node>>(), std::vector<Node> {});
    }
}
