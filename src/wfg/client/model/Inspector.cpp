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

#include <wfg/client/model/Inspector.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <algorithm>
#include <map>

namespace wfg::client::model
{
    namespace
    {
        /*  THE PAGE'S TABLES, TRANSCRIBED (views/inspector.js:100-116). Data,
            not code: reordering a kind is a line here rather than a change to
            anything, which is what made it cheap to argue about with the page
            open - and the same has to be true with the window open. */
        const std::vector<std::string> saidFirst { "number", "name", "colour", "notes" };
        const std::vector<std::string> when      { "preWait", "duration", "postWait" };
        const std::vector<std::string> saidLast  { "enabled", "preset" };

        const std::map<std::string, std::vector<std::string>>& kindOrder()
        {
            static const std::map<std::string, std::vector<std::string>> table
            {
                { "media",   { "file", "level", "startOffset" } },
                { "fade",    { "target", "level", "curve", "points" } },
                { "stop",    { "target", "verb", "curve" } },
                { "osc",     { "address", "value", "wait", "timeout" } },
                { "midi",    { "port", "channel", "type", "number", "data", "sysex", "wait" } },
                { "group",   { "mode", "advance", "selection", "play", "loops", "seed" } },
                { "range",   { "name", "in", "out", "loops" } },
                { "trigger", { "kind", "enabled", "address", "value", "port", "channel",
                               "type", "number", "data", "at" } },
            };

            return table;
        }

        /*  WHAT A ROW IS CALLED ON SCREEN when its own name is not what
            somebody reading it would call it. A short table, and every entry
            here is one the author asked for while using the panel - not a
            translation layer over the parameter table, which would go stale
            the day a row is added by somebody who never opens this file. A
            name that is not here keeps the tree's own word, so nothing can
            vanish by being forgotten. */
        const std::map<std::string, std::string>& labels()
        {
            static const std::map<std::string, std::string> table
            {
                { "play", "items to play" },
            };

            return table;
        }

        bool named (const std::vector<std::string>& names, const std::string& name)
        {
            return std::find (names.begin(), names.end(), name) != names.end();
        }

        Field fieldFrom (const tree::Node& node, const std::string& name)
        {
            Field field;
            field.address = node.address;
            field.name = name;
            field.description = node.description;
            field.unit = node.unit;
            field.typeTags = node.typeTags;
            field.options = node.enumValues;
            field.hasMinimum = node.hasMinimum;
            field.hasMaximum = node.hasMaximum;
            field.minimum = node.minimum;
            field.maximum = node.maximum;

            /*  ASKED OF THE NODE AND NEVER OF A LIST OF NAMES: `ACCESS & write`
                is what makes a row a decision rather than a reading, so a row
                that becomes writable moves out of the fold by itself. */
            field.writable = (static_cast<int> (node.access)
                                & static_cast<int> (tree::Access::write)) != 0;

            field.boolean = node.typeTags == "T" || node.typeTags == "F";
            field.value = text (&node);

            if (const auto found = labels().find (name); found != labels().end())
                field.label = found->second;
            else
                field.label = name;

            /*  WHICH CONTROL ASKS THIS BEST. Decided from the node wherever the
                node can say - a `T` row is a switch, a closed set of values is
                a choice - and NAMED only twice, where no property of a row
                could have said it: `loops`, where one integer carries three
                questions and no single control can put them, and `file`, where
                the value is a name on a disk and a machine can be asked to go
                and find it. Both send the same `node.set` a typed answer would.
                A read-only row is never composed: there is nothing to ask. */
            if (! field.writable)          field.control = Control::text;
            else if (field.boolean)        field.control = Control::toggle;
            else if (! field.options.empty()) field.control = Control::choice;
            else if (name == "loops")      field.control = Control::loopCount;
            else if (name == "file")       field.control = Control::file;
            else                           field.control = Control::text;

            return field;
        }

        void sortInto (std::vector<Field>& fields, const std::vector<std::string>& order)
        {
            /*  NAMED FIRST IN THE ORDER GIVEN, then everything else
                alphabetically: a row nobody has named turns up at the end of
                its own block rather than vanishing or leading. */
            std::stable_sort (fields.begin(), fields.end(),
                              [&order] (const Field& a, const Field& b)
                              {
                                  const auto at = [&order] (const std::string& name)
                                  {
                                      const auto found = std::find (order.begin(), order.end(), name);
                                      return found == order.end()
                                               ? order.size()
                                               : static_cast<std::size_t> (found - order.begin());
                                  };

                                  if (at (a.name) != at (b.name))
                                      return at (a.name) < at (b.name);

                                  return a.name < b.name;
                              });
        }
    }

    Inspection inspect (const tree::TreeSnapshot& snapshot, const std::string& cueId)
    {
        Inspection out;

        if (cueId.empty())
            return out;

        out.cueId = cueId;
        out.cueName = text (snapshot, "/godot/cue/" + cueId + "/name");
        out.kind = text (snapshot, "/godot/cue/" + cueId + "/kind");

        /*  ONE SCAN OF THE TREE, AND ONLY WHEN A SELECTION CHANGES. `all()` is
            linear and allocates, which is why the cue list is forbidden it -
            there it would run per row per pass. Here it runs when somebody
            clicks, which is a few thousand comparisons against a human
            reaction time, and the alternative is this client keeping its own
            copy of which attributes a kind has: exactly the table §14.2 says a
            generic inspector must not have.

            (`childrenOf` would do the same scan and is banned outright, so the
            gate stays honest: what is forbidden is the per-row habit, not the
            one-off.) */
        const auto prefix = "/godot/cue/" + cueId + "/";

        std::vector<Field> decided, reported;

        for (const auto* node : snapshot.all())
        {
            if (node->address.rfind (prefix, 0) != 0)
                continue;

            const auto name = node->address.substr (prefix.size());

            //  Only this cue's own rows: anything deeper belongs to something else.
            if (name.find ('/') != std::string::npos)
                continue;

            auto field = fieldFrom (*node, name);

            (field.writable ? decided : reported).push_back (std::move (field));
        }

        //  The four blocks, in the order somebody fills them in.
        const auto kindRows = [&out]
        {
            const auto found = kindOrder().find (out.kind);
            return found != kindOrder().end() ? found->second : std::vector<std::string> {};
        }();

        Block isBlock { "what it is", {} }, whenBlock { "when", {} },
              doesBlock { "what it does", {} }, listBlock { "in the list", {} };

        for (auto& field : decided)
        {
            if (named (saidFirst, field.name))      isBlock.fields.push_back (std::move (field));
            else if (named (when, field.name))      whenBlock.fields.push_back (std::move (field));
            else if (named (saidLast, field.name))  listBlock.fields.push_back (std::move (field));
            else                                    doesBlock.fields.push_back (std::move (field));
        }

        sortInto (isBlock.fields, saidFirst);
        sortInto (whenBlock.fields, when);
        sortInto (doesBlock.fields, kindRows);
        sortInto (listBlock.fields, saidLast);

        for (auto* block : { &isBlock, &whenBlock, &doesBlock, &listBlock })
            if (! block->fields.empty())
                out.blocks.push_back (std::move (*block));

        sortInto (reported, {});
        out.details = std::move (reported);

        return out;
    }
}
