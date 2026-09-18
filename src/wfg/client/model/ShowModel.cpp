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

#include <wfg/client/model/ShowModel.h>

#include <wfg/client/model/Text.h>
#include <wfg/engine/tree/Node.h>
#include <wfg/engine/tree/TreeSnapshot.h>

#include <string>
#include <string_view>

namespace wfg::client::model
{
    namespace
    {
        /*  HOW DEEP A SHOW MAY NEST BEFORE THIS STOPS WALKING. Not a limit the
            document has - groups nest as far as anybody wants - but a client
            reading a tree it did not build must not be able to be hung by one,
            and a cycle in `order` would otherwise be an infinite walk with a
            window on the other end of it. A show nested sixty-four deep is not
            a show anybody is running, so the cap costs nothing real. */
        constexpr int deepestNesting = 64;

        std::string attribute (const tree::TreeSnapshot& snapshot,
                               const std::string& cueId, const char* name)
        {
            return text (snapshot, "/godot/cue/" + cueId + "/" + name);
        }

        /*  THE PAGE'S RULE FOR A TIME COLUMN, so the two clients spell the same
            number the same way (didi.js:176-196). One decimal, and a READ-ONLY
            nought is blank - otherwise the column says nothing in two ways: an
            empty box where a fade waits for none, and "0.0" where a media
            cue's file could not be read and its length is unknown. A writable
            nought is a decision somebody made and is shown. */
        std::string timeText (const tree::TreeSnapshot& snapshot,
                              const std::string& cueId, const char* name)
        {
            const auto* node = snapshot.find ("/godot/cue/" + cueId + "/" + name);

            if (node == nullptr)
                return {};

            const auto sole = node->soleValue();

            if (! sole.has_value() || ! sole->isNumber())
                return {};

            const auto seconds = sole->asDouble();

            /*  One decimal, written by hand rather than through a locale: a
                stream under fr_FR would put a comma where the page puts a
                point, and the two clients would disagree about a cue's wait. */
            const auto tenths = static_cast<long long> (seconds * 10.0 + (seconds < 0 ? -0.5 : 0.5));
            const auto whole = tenths / 10;
            const auto fraction = tenths % 10;
            const auto written = std::to_string (whole) + "."
                               + std::to_string (fraction < 0 ? -fraction : fraction);

            /*  AND A READ-ONLY NOUGHT IS BLANK, or the column spells nothing in
                two ways: an empty box where a fade waits for none, and "0.0"
                where a media cue's file could not be read and its length is
                unknown.

                THE TEST IS ON THE TEXT, not on the number, and not only to
                keep -Wfloat-equal quiet: what this rule is about is what the
                column SAYS, so asking whether it would say "0.0" is asking the
                question itself. It also takes -0.0 and anything that rounds to
                nothing with it, which an equality against 0.0 would not. */
            const auto writable = (static_cast<int> (node->access)
                                     & static_cast<int> (tree::Access::write)) != 0;

            if (! writable && (written == "0.0" || written == "-0.0"))
                return {};

            return written;
        }
    }

    int ShowModel::indexOf (std::string_view cueId) const
    {
        const auto found = indexOfCue.find (std::string (cueId));
        return found != indexOfCue.end() ? found->second : -1;
    }

    bool ShowModel::refresh (const tree::TreeSnapshot& snapshot, std::string_view listIdToDraw)
    {
        std::uint64_t now = 0;

        if (const auto* node = snapshot.find ("/godot/document/revision"))
            if (const auto sole = node->soleValue(); sole.has_value() && sole->isInt64())
                now = static_cast<std::uint64_t> (sole->getInt64());

        /*  TWO KEYS, because one of them cannot say what the other does. The
            revision moves when the SHOW moves; the focused list is a decision
            about where the operator is standing, which is a state row and
            deliberately does not move it (M0). A model keyed on the revision
            alone would keep drawing the list somebody had just navigated away
            from. */
        /*  A FOLD IS A REASON TO REBUILD that the revision cannot express, for
            the same reason the focused list is: neither is a change to the
            show. */
        if (built && now == revision && listIdToDraw == listId && ! foldsMoved)
            return false;

        foldsMoved = false;
        revision = now;
        listId = std::string (listIdToDraw);
        built = true;
        ++walks;

        drawn.clear();
        indexOfCue.clear();

        if (! listId.empty())
            walk (snapshot, listId, true, 0);

        return true;
    }

    void ShowModel::walk (const tree::TreeSnapshot& snapshot, const std::string& container,
                          bool isList, int depth)
    {
        if (depth > deepestNesting)
            return;

        const auto address = (isList ? "/godot/list/" : "/godot/cue/") + container + "/";

        const auto members = [&snapshot, &address] (const char* name)
        {
            return words (text (snapshot, address + name));
        };

        /*  THE PERSISTENT BAND FIRST, at the top of the list, because it is
            what runs as soon as the show starts - the author's own correction
            to the page, made when they first saw it drawn at the bottom. A
            group has no persistent section; a list has no header or footer of
            its own. */
        if (isList)
            section (snapshot, container, members ("persistentOrder"),
                     Section::persistent, "persistent", depth);
        else
            section (snapshot, container, members ("headerOrder"),
                     Section::header, "header", depth);

        for (const auto& id : members ("order"))
            append (snapshot, id, Section::member, depth, container);

        if (! isList)
            section (snapshot, container, members ("footerOrder"),
                     Section::footer, "footer", depth);
    }

    void ShowModel::section (const tree::TreeSnapshot& snapshot, const std::string& container,
                             const std::vector<std::string>& ids, Section which,
                             const char* word, int depth)
    {
        /*  NOTHING IS FRAMED WHEN THERE IS NOTHING TO FRAME. An empty section
            is not a thing an operator needs told about; the page drops its band
            for the same reason. */
        if (ids.empty())
            return;

        Row head;
        head.rowKind = RowKind::band;
        head.section = which;
        head.depth = depth;
        head.parent = container;
        head.name = word;
        head.count = ids.size();
        head.bandKey = container + "/" + word;
        head.shut = folded.count (head.bandKey) != 0;

        drawn.push_back (std::move (head));

        if (drawn.back().shut)
            return;

        for (const auto& id : ids)
            append (snapshot, id, which, depth, container);
    }

    void ShowModel::toggle (const std::string& bandKey)
    {
        if (folded.count (bandKey) != 0)
            folded.erase (bandKey);
        else
            folded.insert (bandKey);

        foldsMoved = true;
    }

    bool ShowModel::isShut (const std::string& bandKey) const
    {
        return folded.count (bandKey) != 0;
    }

    void ShowModel::append (const tree::TreeSnapshot& snapshot, const std::string& cueId,
                            Section section, int depth, const std::string& parent)
    {
        /*  A CUE DRAWN TWICE IS A DOCUMENT THAT DISAGREES WITH ITSELF, and a
            client that followed it would walk forever. The first placement
            wins and the second is dropped: a row that cannot be reached is
            better than a window that does not come back. */
        if (indexOfCue.count (cueId) != 0)
            return;

        Row row;
        row.id = cueId;
        row.name = attribute (snapshot, cueId, "name");
        row.kind = attribute (snapshot, cueId, "kind");
        row.number = attribute (snapshot, cueId, "number");
        row.preset = attribute (snapshot, cueId, "preset");
        row.preWait = timeText (snapshot, cueId, "preWait");
        row.duration = timeText (snapshot, cueId, "duration");
        row.postWait = timeText (snapshot, cueId, "postWait");
        row.depth = depth;
        row.section = section;
        row.parent = parent;
        row.enabled = flag (snapshot, "/godot/cue/" + cueId + "/enabled") != Flag::no;

        /*  WHAT MAKES A ROW A GROUP is that it has members to draw, which is
            the question this file is asking - not that its `kind` reads
            "group". They are the same thing today; asking the structure means
            a kind added later that also holds cues draws correctly without
            this file being edited. */
        row.isGroup = snapshot.find ("/godot/cue/" + cueId + "/order") != nullptr;

        if (row.isGroup)
            row.mode = attribute (snapshot, cueId, "mode");

        /*  A GROUP FOLDS LIKE A SECTION DOES, and by its own identifier: it
            holds cues, so an operator reading a long show wants it shut as
            much as they want a footer shut. Its head is its own row rather
            than a band, which is why the key is the cue's id and not a
            container-and-word pair. */
        if (row.isGroup)
        {
            row.bandKey = cueId;
            row.shut = folded.count (cueId) != 0;
        }

        const auto walkInto = row.isGroup && ! row.shut;

        indexOfCue.emplace (cueId, static_cast<int> (drawn.size()));
        drawn.push_back (std::move (row));

        if (walkInto)
            walk (snapshot, cueId, false, depth + 1);
    }
}
