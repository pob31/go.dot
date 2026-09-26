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

#include <wfg/engine/surface/SurfaceCommands.h>

#include <wfg/engine/osc/OscValue.h>

#include <string>

namespace wfg::surface
{
    void registerSurfaceCommands (CommandRegistry& registry, doc::ShowDocument& document,
                                  SurfaceTable& table)
    {
        registry.add ({ "surface.aim",
                        "Chooses the media cue a surface's rotaries edit on its EQ and Send pages,"
                        " or none with an empty argument. One cue for every surface.",
                        { { "cue", 's', false } },
                        true,
                        [&document, &table] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto id = args[0].getString();

                            /*  EMPTY IS NONE, and applied: letting go of the
                                aim is a thing a hand does - a second press of
                                a lit SELECT - not a mistake. */
                            if (id.empty())
                            {
                                table.setAim ({});
                                return Outcome::ok (args);
                            }

                            const auto cue = document.findById (id);

                            if (! cue.isValid())
                                return Outcome::rejected (reason::unknownId);

                            /*  A MEDIA CUE, OR A MIC CUE (Phase 9b): both carry
                                the EQ, the sends and the inserts the pages
                                put under the rotaries. */
                            if (! cue.hasType ("Media") && ! cue.hasType ("Mic"))
                                return Outcome::rejected (reason::badValue);

                            table.setAim (id);
                            return Outcome::ok (args);
                        } });

        /*  THE MASTER DIAL (author, 2026-09-26: "Can selecting a parameter in
            the inspector or foot panel on-screen via mouse or touch assign it
            to the master rotary encoder on the D700?"). Any click or touch on
            a number the window draws sends this; the D700's click on the dial
            sends it empty. It stays on that cue's row when the pick moves.

            ONLY A NUMBER SOMEBODY DECIDES: a row the show stores and a client
            may write, one value, a number or a whole number, not a closed set.
            A name, a switch, a menu or a reading has nothing a turn could mean
            and is refused `bad-value`; an address that names nothing is
            `bad-address`. Which way the number moves is read off its row here,
            once, so the bridge turns it on the tick without the document. */
        registry.add ({ "surface.dial",
                        "Puts a number on the control surfaces' master dial by its address, or"
                        " frees the dial with an empty argument. One for every surface.",
                        { { "address", 's', false } },
                        true,
                        [&document, &table] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            const auto address = args[0].getString();

                            if (address.empty())
                            {
                                table.setDial ({});
                                return Outcome::ok (args);
                            }

                            const auto resolved = document.resolve (address);

                            if (! resolved.isValid())
                                return Outcome::rejected (reason::badAddress);

                            const auto& row = *resolved.attribute;
                            const auto number = row.type() == doc::ValueType::number
                                             || row.type() == doc::ValueType::integer;

                            if (resolved.isDerived || row.access() != doc::Access::readWrite
                                  || row.persist() != doc::Persist::show
                                  || ! number || row.isList() || row.isEnum())
                                return Outcome::rejected (reason::badValue);

                            SurfaceTable::Dial dial;
                            dial.address = address;
                            dial.integer = row.type() == doc::ValueType::integer;
                            dial.hasMinimum = row.row->hasMin;
                            dial.minimum = row.row->minimum;
                            dial.hasMaximum = row.row->hasMax;
                            dial.maximum = row.row->maximum;
                            dial.unit = std::string (row.row->unit);

                            if (row.hasDefault())
                                if (const auto rest = osc::parseDouble (row.defaultText()))
                                {
                                    dial.hasRest = true;
                                    dial.rest = *rest;
                                }

                            table.setDial (std::move (dial));
                            return Outcome::ok (args);
                        } });
    }
}
