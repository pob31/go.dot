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

#pragma once

/*
    THE OUTPUT LAYOUT: which buses the show has, in which order, how wide each
    one is — and what that does to the interface patch.

    A show's outputs are a list somebody wrote: so many mono direct outs, so
    many stereo mix channels, interleaved however the rig is wired (PRD §6.2's
    own example is thirty-two mono direct outs interleaved with sixteen stereo
    buses on one sixty-four channel interface). `Bus/@firstChannel` is not a
    number anybody types: it is the running sum of the widths before it, and
    keeping it that way is this file's job.

    WHY IT IS NOT WRITABLE AT THE DOOR. `firstChannel` and `width` are
    `access=r` rows, so `node.set` refuses them and every change comes through
    `bus.create`, `bus.delete`, `bus.move` or `bus.width`. A client that could
    write `firstChannel` could leave two buses overlapping — the same hardware
    channel summing two different mixes — and nothing downstream would notice
    until somebody heard it. One door, one rule, one place to read it.

    AND WHY THE PATCH IS PART OF THE SAME QUESTION. `audio/@outputPatch` names
    an interface channel per logical output, and an EMPTY patch is identity:
    logical n goes to interface n. So while the patch is empty, the layout IS
    the patch, and adding a stereo mix at the top of the list silently moves
    every output below it by two. That is exactly right while a show is being
    written — the designer is arranging the interface, and the arrangement
    should follow — and exactly wrong once somebody has patched by hand or
    heard the rig, when what an output is plugged into has become a fact about
    the building rather than a consequence of a list.

    THE RULE, WHICH IS WFS-DIY'S (`WFSValueTreeState::compactInputPatchToDisplay
    Order`, and the `channelNumbersUserOwned` latch beside it, read 2026-09-21):

      - While the show is FRESH — nobody has edited the patch, nothing has
        played, and the layout as loaded is packed — the patch stays EMPTY and
        follows the list. Every edit simply re-flows it, and the operator is
        told so in words.
      - Once it has SETTLED — `audio/@patchSettled`, or a patch string that is
        already there, or a layout that arrived unpacked — the patch is
        materialised for the layout as it was, and then each edit moves rows
        rather than re-deriving them: a new bus takes the next interface
        channels past the highest one anything uses, a deleted bus drops its
        block, a moved bus carries its block with it, a widened bus appends and
        a narrowed one drops.

    A LAYOUT THAT ARRIVED UNPACKED IS SOMEBODY'S DECISION, NOT A MISTAKE.
    `tests/fixtures/bundles/slots` declares a twelve-wide processor send at
    channel 8 and a foldback at 0 — out of document order, with a hole. That is
    a rig, and repacking it would move a processor feed. So an unpacked layout
    counts as settled: the first command materialises the channels it actually
    has into the patch, and only then repacks. Nothing anybody wrote is lost;
    it moves from `firstChannel`, where it can no longer be expressed once the
    list is packed, into the patch, which is where "this output is plugged in
    there" belongs.

    Vendor-free and pure: it takes the layout and the patch as plain numbers and
    answers with new ones. It touches no document, so a test can hand it the
    awkward cases directly, and `ShowDocument` is left with nothing to do but
    read, call and write.
*/

#include <cstddef>
#include <string>
#include <vector>

namespace wfg::doc
{
    /*  One bus, as this file needs it: who it is, how wide, and where it
        starts today. The name and the kind are the document's business and
        never this file's — moving a mix channel and moving a direct out are
        the same arithmetic. */
    struct BusShape
    {
        std::string id;
        int width = 1;
        int firstChannel = 0;
    };

    /*  What somebody asked for. `index` is a position in the list of buses,
        counted the way a client counts rows; -1 appends. `width` is read for
        `create` and `resize` and ignored otherwise; `id` is ignored for
        `create`, whose new bus is described by `index` and `width` alone and
        named by the caller. */
    struct LayoutEdit
    {
        enum class Kind { create, remove, move, resize };

        Kind kind = Kind::create;
        std::string id;
        int index = -1;
        int width = 1;
    };

    struct OutputLayout
    {
        /*  The buses in their new order, each with its repacked
            `firstChannel`. For a `create` the new bus is in here with an EMPTY
            identifier, at the position it was asked for: the caller draws the
            name, and this file has no business inventing one. */
        std::vector<BusShape> buses;

        /*  The new patch, as a channel per logical output. EMPTY means "still
            following the layout", which is what the device layer already reads
            as identity — so an empty answer here is an instruction to leave
            `outputPatch` alone rather than to write an empty string over
            something. `patchChanged` tells the two apart. */
        std::vector<int> outputPatch;
        bool patchChanged = false;

        /*  Why the edit could not be made, in words; empty when it was. A
            refusal leaves every other field untouched, so a caller that checks
            this first cannot half-apply anything. */
        std::string problem;

        /** The new index of the bus the edit named; -1 when there is none. */
        int placedAt = -1;
    };

    /** Whether every bus starts where the widths before it say it should. */
    bool isPacked (const std::vector<BusShape>& buses);

    /*  The layout after the edit, and the patch that goes with it.

        `before` is every bus the show has, in the order the list is read —
        which is `firstChannel` order, ties broken by document order. `patch` is
        `audio/@outputPatch` as it stands, empty for "following". `settled` is
        `audio/@patchSettled`; the patch is treated as settled when that flag is
        true, when `patch` is not empty, or when `before` is not packed. */
    OutputLayout applyLayoutEdit (std::vector<BusShape> before,
                                  const std::vector<int>& patch,
                                  bool settled,
                                  const LayoutEdit& edit);
}
