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

#include "SectionCommands.h"

#include <cmath>
#include <utility>

namespace wfg::doc
{
    namespace
    {
        Outcome fromEdit (const EditResult& edit, std::vector<osc::Value> appliedArgs)
        {
            if (! edit.ok)
                return Outcome::rejected (edit.reason);

            return Outcome::ok (std::move (appliedArgs));
        }

        /*  The record carries what the session drew or knew, at `index`, so a
            replay draws nothing and reads it there. */
        std::vector<osc::Value> withValue (std::vector<osc::Value> args, std::size_t index, osc::Value value)
        {
            while (args.size() < index)
                args.push_back (osc::Value::string ({}));

            if (args.size() > index)
                args[index] = std::move (value);
            else
                args.push_back (std::move (value));

            return args;
        }

        std::string stringAt (const std::vector<osc::Value>& args, std::size_t index)
        {
            return args.size() > index && args[index].isString() ? args[index].getString() : std::string {};
        }

        double numberAt (const std::vector<osc::Value>& args, std::size_t index)
        {
            return args.size() > index && args[index].isNumber() ? args[index].asDouble() : 0.0;
        }
    }

    void registerSectionCommands (CommandRegistry& registry, ShowDocument& document, FileLengthOf lengthOf)
    {
        //----------------------------------------------------------------------
        registry.add ({ "section.split",
                        "Cuts a sound's edited timeline at a second of it (namespace draft 55): the piece"
                        " that second falls in becomes two, the second half a new section directly after"
                        " the first, with the first's trim. A cue with no sections yet first gets one over"
                        " the whole file, as long as the session knows the file to be - written back on the"
                        " record, so a replay reads it there. On a cut, at the top or at the end there is"
                        " nothing to divide.",
                        { { "cue", 's', false }, { "at", 'd', false }, { "id", 's', true }, { "length", 'd', true } },
                        true,
                        [&document, lengthOf] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto cueId = stringAt (args, 0);
                            const auto at = numberAt (args, 1);
                            const auto id = stringAt (args, 2);
                            auto length = numberAt (args, 3);

                            if (! (length > 0.0) && lengthOf)
                                if (const auto cue = document.findById (cueId); cue.isValid())
                                    length = lengthOf (cue["file"].toString().toStdString());

                            const auto edit = document.splitSection (cueId, at, length, id);

                            auto applied = withValue (args, 2, osc::Value::string (edit.id));
                            applied = withValue (std::move (applied), 3, osc::Value::float64 (length));

                            return fromEdit (edit, std::move (applied));
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.join",
                        "Takes a cut back: the section and the one after it made one again, when the two"
                        " are still one in the file. The survivor keeps its trim and its crossfade.",
                        { { "section", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.joinSection (stringAt (args, 0)), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.trim",
                        "Moves a section's edges, in seconds of the file. Everything after it on the"
                        " edited timeline shifts, and the cue's lane points, its ranges and its start"
                        " offset are carried along in the same edit; what sat on a sliver cut away goes"
                        " with it.",
                        { { "section", 's', false }, { "in", 'd', false }, { "out", 'd', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.trimSection (stringAt (args, 0), numberAt (args, 1), numberAt (args, 2)),
                                             args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.move",
                        "Moves a section to a place among its cue's sections, counting from nought; the"
                        " lane points, the ranges and the start offset go with the sound. A range left"
                        " ending before it begins - the two sides of the move - is taken out.",
                        { { "section", 's', false }, { "index", 'i', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.moveSection (stringAt (args, 0),
                                                                   static_cast<int> (std::lround (numberAt (args, 1)))),
                                             args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.remove",
                        "Takes a section out of the edit: what sat on it goes with it and the rest closes"
                        " up. With the last section gone the cue plays its whole file again, and every"
                        " point is carried back to the file's own time.",
                        { { "section", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.removeSection (stringAt (args, 0)), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "section.clear",
                        "The edit gone: every section taken out, the cue playing its whole file again,"
                        " its lane points, ranges and start offset carried back to the file's own time.",
                        { { "cue", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.clearSections (stringAt (args, 0)), args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "media.frozen",
                        "The bounce is there (namespace draft 55, ADN): the cue plays it - file becomes the"
                        " bounce's name, editSource keeps the file the edit was made from - and its"
                        " sections wait for media.unfreeze. Submitted by the renderer once media.freeze's"
                        " copy has landed; one undoable edit, and the bounce stays on disk when it is undone.",
                        { { "cue", 's', false }, { "source", 's', false }, { "bounce", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.freezeEdit (stringAt (args, 0), stringAt (args, 1), stringAt (args, 2)),
                                             args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "media.unfreeze",
                        "The cue pointed back at the file its edit was made from; the sections are live"
                        " again and the render follows them.",
                        { { "cue", 's', false } },
                        true,
                        [&document] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            return fromEdit (document.unfreezeEdit (stringAt (args, 0)), args);
                        } });
    }
}
