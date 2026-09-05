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
        std::string orderOf (const juce::ValueTree& node)
        {
            std::string out;

            for (const auto& child : node)
            {
                if (! child.hasProperty (idProperty))
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

        void collectCue (const juce::ValueTree& node, const std::string& parentId, int index,
                         std::vector<Node>& out)
        {
            const auto element = node.getType().toString().toStdString();
            const auto isGroup = element == "Group";
            const auto isMedia = element == "Media";
            const auto id = node[idProperty].toString().toStdString();

            if (id.empty())
                return;

            const auto base = std::string (godot) + "/cue/" + id;

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

            for (const auto* row : rows)
            {
                const doc::Attribute attribute { element, row };
                const auto name = std::string (row->name);

                std::string text;

                /*  Derived from the element, never stored - which is what makes
                    it read-only in a way a client cannot argue with. */
                if (name == "kind")        text = isGroup ? "group" : isMedia ? "media" : "memo";
                else if (name == "parent") text = parentId;
                else if (name == "index")  text = std::to_string (index);
                else if (name == "order")  text = orderOf (node);
                else                       text = storedText (attribute, node);

                out.push_back (makeLeaf (base + "/" + name, *row, text));
            }

            int childIndex = 0;

            for (const auto& child : node)
            {
                if (! child.hasProperty (idProperty))
                    continue;

                /*  A ROUTE IS NOT A NESTED CUE, and the recursion below would
                    have made it one - it takes any identified child. A
                    destination has its own top-level address so that changing
                    one gain is a write to one node rather than a rewrite of the
                    cue's whole routing (author, 2026-09-05). */
                if (child.getType().toString() == "Route")
                {
                    collectRoute (child, out);
                    continue;
                }

                collectCue (child, id, childIndex++, out);
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
        for (const auto& container : showNode)
        {
            const auto containerName = container.getType().toString().toStdString();

            if (containerName == "Lists")
            {
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
                        const auto text = name == "order" ? orderOf (list)
                                                          : storedText (attribute, list);

                        nodes.push_back (makeLeaf (base + "/" + name, *row, text));
                    }

                    int index = 0;

                    for (const auto& cue : list)
                        if (cue.hasProperty (idProperty))
                            collectCue (cue, id, index++, nodes);
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
                        else
                            text = storedText (attribute, mount);

                        nodes.push_back (makeLeaf (base + "/" + name, *row, text));
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
            }
        }

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

        /*  Somebody else's namespace, at its own prefix. It is part of the
            document half rather than the runtime half because it changes only
            when a mount is loaded or written to, both of which mark the tree
            stale - and because a show with four mounted processors has rather
            more mounted nodes than it has cues. */
        for (auto& mounted : mounts.allNodes())
            nodes.push_back (std::move (mounted));

        addContainers (nodes, {}, true);
        sortByAddress (nodes);

        documentPart = std::make_shared<const std::vector<Node>> (std::move (nodes));
        stale = false;
    }

    //==============================================================================
    std::shared_ptr<const TreeSnapshot> ParameterTree::publish (std::int64_t tick,
                                                                const EngineState& state)
    {
        if (stale || documentPart == nullptr)
            rebuildDocumentPart();

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
            else if (name == "rtViolations")   text = std::to_string (state.rtViolations);
            else if (name == "rtForeignAllocations")
                                               text = std::to_string (state.rtForeignAllocations);
            else if (name == "lastError")      text = state.lastError;
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

            Finished runs are published too, and keep their addresses. A client
            that asked what happened can still be told, and Phase 3 is where
            pruning becomes a real question because a group's several runs make
            "which one" worth asking. */
        for (const auto& run : runs.all())
        {
            if (run.id.empty())
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
                else if (name == "error")     text = run.error;
                else                          text = std::string (row->defaultText);

                runtime.push_back (makeLeaf (base + "/" + name, *row, text));
            }
        }

        /*  "/", "/godot" and "/godot/document" belong to the document half, so
            the runtime half must not carry them too - `find` looks in one and
            then the other, and a duplicate would make the answer depend on
            which it reached first. What is left for this side to add is
            "/godot/engine". */
        addContainers (runtime,
                       { std::string (rootAddress), std::string (godot),
                         std::string (godot) + "/document",
                         std::string (godot) + "/audio" },
                       false);
        sortByAddress (runtime);

        auto result = std::make_shared<const TreeSnapshot> (tick, documentPart, std::move (runtime));

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
            -1, std::make_shared<const std::vector<Node>>(), std::vector<Node> {});
    }
}
