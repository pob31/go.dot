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

#include <wfg/engine/cue/Solver.h>

#include <cctype>

#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/Schema.h>

#include <algorithm>
#include <set>

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

        /** The child identifiers of a container, in order, space-separated. */
        /*  The identifiers of a container's children, in order, space-separated.

            `only` narrows it to one element name, which is what a group's
            header and footer need: `order` is the group's MEMBERS and a header
            is not one of them - a client reading `order` is reading the cue
            list, and the two cues that run before and after it are a different
            question with two nodes of their own. */
        std::string orderOf (const juce::ValueTree& node, const char* only = nullptr)
        {
            std::string out;

            for (const auto& child : node)
            {
                if (! child.hasProperty (idProperty))
                    continue;

                const auto element = child.getType().toString();

                if (only != nullptr ? element != only
                                    : (element == "Header" || element == "Footer"
                                        || element == "Persistent"))
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
                          std::vector<Node>& out)
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

        void collectCue (const juce::ValueTree& node, const std::string& parentId, int index,
                         std::vector<Node>& out,
                         const std::map<std::string, double>* durations,
                         std::vector<std::string>& roster,
                         const char* role = "member")
        {
            const auto element = node.getType().toString().toStdString();
            const auto isGroup = element == "Group";
            const auto isMedia = element == "Media";
            const auto isFade = element == "Fade";
            const auto isStop = element == "Stop";
            const auto isOsc = element == "Osc";
            const auto isMidi = element == "Midi";
            const auto id = node[idProperty].toString().toStdString();

            if (id.empty())
                return;

            const auto base = std::string (godot) + "/cue/" + id;

            /*  KEPT FOR THE RUNTIME HALF, which has no document to walk. See
                `ParameterTree::declaredCues`. */
            roster.push_back (id);

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
                for (auto* row : doc::Schema::rowsForOwner ("stop"))
                    rows.push_back (row);

            if (isOsc)
                for (auto* row : doc::Schema::rowsForOwner ("osc"))
                    rows.push_back (row);

            if (isMidi)
                for (auto* row : doc::Schema::rowsForOwner ("midi"))
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

                std::string text;

                /*  Derived from the element, never stored - which is what makes
                    it read-only in a way a client cannot argue with. */
                if (name == "kind")        text = isGroup ? "group"
                                                  : isMedia ? "media"
                                                  : isFade  ? "fade"
                                                  : isStop  ? "stop"
                                                  : isOsc   ? "osc"
                                                  : isMidi  ? "midi"
                                                            : "memo";
                else if (name == "parent") text = parentId;
                else if (name == "index")  text = std::to_string (index);
                else if (name == "role")   text = role;
                else if (name == "duration")
                {
                    /*  READ ONCE WHEN THE SHOW WAS OPENED, and nought when
                        nobody read any: a replay, a tree dump of a bundle with
                        no media folder, a test. Nought is the same answer a
                        missing or unreadable file gives, and §3.13's solver
                        reads it as "I do not know how long this is" rather than
                        as "this has ended". */
                    const auto named = node[juce::Identifier ("file")].toString().toStdString();
                    const auto found = durations != nullptr ? durations->find (named)
                                                            : std::map<std::string, double>::const_iterator {};

                    text = (durations != nullptr && found != durations->end())
                             ? osc::formatDouble (found->second)
                             : osc::formatDouble (0.0);
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
                if (childElement == "Feed" || childElement == "Insert")
                {
                    const auto* owner = childElement == "Feed" ? "feed" : "insert";
                    const auto childId = child[idProperty].toString().toStdString();

                    if (! childId.empty())
                    {
                        const auto childBase = std::string (godot) + "/"
                                                 + std::string (owner) + "/" + childId;

                        for (const auto* row : doc::Schema::rowsForOwner (owner))
                        {
                            const doc::Attribute attribute { childElement.toStdString(), row };
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
                    collectTrigger (child, id, out);
                    continue;
                }

                if (childElement == "Header" || childElement == "Footer")
                {
                    int roleIndex = 0;
                    const auto* childRole = childElement == "Header" ? "header" : "footer";

                    for (const auto& roleChild : child)
                        if (roleChild.hasProperty (idProperty))
                            collectCue (roleChild, id, roleIndex++, out, durations, roster, childRole);

                    continue;
                }

                collectCue (child, id, childIndex++, out, durations, roster);
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
        // /godot/document — the show format's own version. The rest of that
        // container is runtime state and lives on the other side.
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

        /*  And every list, for `aim`, `solve` and `statePosition` - three more
            answers that are about a session rather than about a show. */
        std::vector<std::string> listOrder;

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
                            collectCue (cue, id, index++, nodes, durations, cueOrder);
                    }

                    if (const auto section = list.getChildWithName ("Persistent");
                        section.isValid())
                    {
                        int persistentIndex = 0;

                        for (const auto& cue : section)
                            if (cue.hasProperty (idProperty))
                                collectCue (cue, id, persistentIndex++, nodes, durations,
                                            cueOrder, "persistent");
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
                /*  THE PORTS THE SHOW DECLARES, and nothing about which cable
                    each one is. A Port carries a name somebody chose - "Lights",
                    "The desk" - and what it is bound to on this machine is
                    `--midi-out`'s answer and never the document's (§4.10). */
                for (const auto& port : container)
                {
                    const auto id = port[idProperty].toString().toStdString();

                    if (id.empty())
                        continue;

                    const auto base = std::string (godot) + "/port/" + id;

                    for (const auto* row : doc::Schema::rowsForOwner ("port"))
                    {
                        const doc::Attribute attribute { "Port", row };

                        nodes.push_back (makeLeaf (base + "/" + std::string (row->name),
                                                   *row, storedText (attribute, port)));
                    }
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

                        nodes.push_back (makeLeaf (base + "/" + std::string (row->name),
                                                   *row, storedText (attribute, bus)));
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
        declaredLists = std::move (listOrder);

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

        addContainers (nodes, {}, true);
        sortByAddress (nodes);

        documentPart = std::make_shared<const std::vector<Node>> (std::move (nodes));
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
        analysis.ensureBuilt (document, durations);

        if (stale || documentPart == nullptr)
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
            else                         text = std::string (row->defaultText);

            engineValue (*row, "audio", text);
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
                const auto question = std::to_string (document.revision()) + " "
                                        + cue::spellAim (aim);

                if (solvedFor[listId] != question)
                {
                    solvedFor[listId] = question;

                    solves[listId] = aim.isSet()
                                       ? cue::solve (document, durations, &mounts,
                                                     { listId, aim.cue, aim.offset }).toJson()
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
                else if (name == "level")     text = osc::formatDouble (run.level);
                else if (name == "late")      text = std::to_string (run.late);
                else if (name == "parent")    text = run.parent;
                else if (name == "children")  text = joinIds (run.children);
                else if (name == "phase")     text = run.phase;
                else if (name == "claims")    text = joinIds (run.claims);
                else if (name == "pending")   text = joinIds (run.pending);
                else if (name == "warning")   text = run.warning;
                else if (name == "asserted")  text = run.asserted ? "true" : "false";
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
            visible. */
        std::vector<std::string> ownedByTheDocument {
            std::string (rootAddress), std::string (godot),
            std::string (godot) + "/document",
            std::string (godot) + "/audio",
            std::string (godot) + "/cue",
            std::string (godot) + "/list",
            std::string (godot) + "/slot" };

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
