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
    One node of the parameter tree, as a client sees it.

    A PROJECTION, NOT A SECOND COPY. Every value here comes from somewhere that
    already owns it - an attribute in show.xml, a counter in the engine, a
    command in the registry - and the tree holds it only for as long as one
    snapshot lives. Nothing writes back through a Node; a write is the
    `node.set` command, which goes to the document's own write path like every
    other mutation (PRD §4.11).

    PLAIN DATA ON PURPOSE. No juce::var, no ValueTree handle, no pointer into
    the document. Snapshots are read from server threads while the tick thread
    is already building the next one, so a Node that referred to anything the
    tick thread owns would be a data race dressed as a struct. The one pointer
    it holds, to its `NodeShape`, is to something nobody writes after it is
    made: the shape is shared by every node of one row, and never changed.

    THE FOUR THINGS PRD §3.3 SAYS A NODE DECLARES are `kind`, `rateCap`,
    `anticipatable` and `panic`. They travel to a client in one vendor key,
    `GODOT`, because the OSCQuery proposal makes custom attributes
    "intentionally trivial" and a client that does not know the key ignores it.
    Every one of them comes from a column of the parameter table.
*/

#include <wfg/engine/osc/OscValue.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wfg::tree
{
    /*  PRD §3.3's distinction, and it is not cosmetic: settable state has a
        value at any given time and can therefore be solved for, saved and
        recalled, while an event is one-shot and has none. A container holds
        other nodes and nothing else. */
    enum class Kind { container, state, event };

    /*  OSCQuery's ACCESS numbers, spelled as the wire spells them so that
        nothing has to translate at the boundary. */
    enum class Access
    {
        none      = 0,      ///< a container
        read      = 1,
        write     = 2,      ///< a command: invoke it, never read it
        readWrite = 3
    };

    /*  HOW A NODE IS SPELLED ON THE MIDI WIRE (namespace draft §57, AFJ;
        DP.9), from `GODOT.MIDI`, with fixed keys and no expression strings:
        KIND pc (a Program Change, Bank Select first when BANKED, the number
        counted from START, PROGRAM the node's own or -1 for the atom's),
        note (a Note On at ON when the atom is on and OFF when it is off, then
        its release when RELEASE), cc (CC with the atom), nrpn (MSB and LSB
        the parameter, the data in BITS - 7 with FINE as the data LSB where
        given, 14 split - or ON and OFF as two codes when the atom is a
        switch), sysex (BYTES as hex and the tokens N, N+k, V, V14, S) and
        msc (COMMAND, the device and format from the mount). CHANNEL fixes
        the MIDI channel; OFFSET adds to the mount's. Plain data, as the
        node is. */
    struct MidiShape
    {
        std::string kind;
        int channel = 0;
        int offset = 0;
        int program = -1;
        int start = 0;
        bool banked = false;
        int note = 0;
        int on = 127;
        int off = 0;
        bool hasOnOff = false;
        bool release = true;
        int cc = 0;
        int msb = 0;
        int lsb = 0;
        int bits = 7;
        int fine = -1;
        std::vector<std::string> bytes;
        int command = 0;
    };

    /*  The bounds of one argument of a node, from the table or a device's
        file: unbounded where none were given. */
    struct ArgumentRange
    {
        bool hasMinimum = false;
        double minimum = 0.0;
        bool hasMaximum = false;
        double maximum = 0.0;
    };

    /*  WHAT A NODE SAYS ABOUT ITSELF, as against what it says NOW
        (2026-10-11, §31.5): everything a client is told about a node that is the
        same for every node made from one row of the parameter table, or from
        one node of a device's file - its sentence, its unit, its range and
        enumeration, PRD §3.3's declarations and the wires' spellings. The
        address and the value are the node's own and stay on it.

        SHARED, NEVER COPIED. The tree publishes every tick, and it used to
        copy all of this into every leaf it made: a run's forty-odd rows, each
        carrying a description of up to hundreds of characters, for every run,
        fifty times a second - most of an idle tick in the Debug build CI
        runs, and on a runner slower than the bench a tick that outlasted a
        tick (blackbox.first-sound, 2026-10-11). A row's shape is made once
        for the process (ParameterTree.cpp, `rowLeaves`), a device node's once
        when its file is read (Mount.cpp), and a node holds it by a pointer to
        something nobody writes again, which is as safe to read from a server
        thread as the rest of a published snapshot. Plain data otherwise, as
        the node is. */
    struct NodeShape
    {
        /** One sentence, from the parameter table. What a client shows at 2 a.m. */
        std::string description;

        /*  WHAT THE NODE IS FOR, in the vocabulary every preset shares
            (namespace draft §57, AFM): `scene.recall`, `strip.level`,
            `object.position`... from the description's `GODOT.ROLE`; empty for
            a node that claims none, which is most of them. Published back as
            `ROLE`, so a client names it and a later round retargets by it. */
        std::string role;

        /*  A PART OF THE TREE THAT TAKES WHAT IT DOES NOT LIST (the author,
            2026-10-24: "For the EOS preset, the cues can be decimal"), from
            `GODOT.OPEN`: an address under this node that the file names no
            node for goes out as the cue spells it, as an opaque device's
            does - a description can list cue 98 and never cue 98.1.

            AND A LEVEL THAT MAY BE LEFT OUT, from `GODOT.SKIP`: the parts
            under this node are optional, and an address that leaves one out
            goes on as though under the first of them - an Eos cue fired in
            the console's current list, `/eos/cue/98.1/fire`, beside one in a
            list named, `/eos/cue/2/98.1/fire`. Read by the menus only. */
        bool open = false;
        bool skips = false;

        /*  HOW THE NODE IS SPELLED ON THE RCP WIRE (namespace draft §57, AFH;
            DP.7), from `GODOT.RCP`: the verb - `set`, or a scene recall's
            own - and how many of the address's trailing segments are the
            console's X and Y; -1 where the file said nothing and the wire
            infers them from the segments that are whole numbers. */
        std::string rcpVerb;
        int rcpIndexes = -1;

        /*  AND ON THE LINE WIRE (DP.8), from `GODOT.LINE`: the command line
            the node renders to, `{x}` and `{y}` the address's first two
            whole-number segments, `{1}` to `{9}` the atoms; empty, the atoms
            are the line. */
        std::string lineTemplate;

        /*  AND ON THE MIDI WIRE (DP.9), from `GODOT.MIDI`: empty kind, none. */
        MidiShape midi;

        //======================================================================
        // The declared range, when the table gives one.
        bool hasMinimum = false;
        double minimum = 0.0;
        bool hasMaximum = false;
        double maximum = 0.0;

        /** A closed set of legal values, when the table declares one. */
        std::vector<std::string> enumValues;

        /*  THE RANGE OF EACH ARGUMENT AFTER THE FIRST, for a node that takes
            several - `/adm/obj/1/xyz` has three, each with its own bounds
            (namespace draft §45). The first stays in `hasMinimum` and its
            neighbours above, which every node of Go.dot's own is read by;
            this is empty for a node of one argument, and holds one entry per
            later type tag where a description gave them. `rangeOf` reads
            either. */
        std::vector<ArgumentRange> laterRanges;

        /** The bounds of one argument, from nought: unbounded where none were given. */
        ArgumentRange rangeOf (std::size_t argument) const
        {
            if (argument == 0)
                return { hasMinimum, minimum, hasMaximum, maximum };

            if (argument - 1 < laterRanges.size())
                return laterRanges[argument - 1];

            return {};
        }

        /** `s`, `Hz`, `dB`, `samples`, ... Empty when the value is not a quantity. */
        std::string unit;

        //======================================================================
        // The GODOT key: PRD §3.3's four declarations.
        double rateCap = 50.0;
        bool anticipatable = false;

        /*  WHAT THE NODE RESTS AT (PRD §4.6), as one of three words: "park"
            (it stays where it is), "snap", or "value" - it rests at the value
            in `panicValues`. A policy is a word and a value is a value, so the
            two are kept apart rather than one string that might be either: a
            string node resting at the text "park" would otherwise read as the
            policy. Nothing APPLIES any of them, nor will (namespace draft §73,
            2026-10-16): a double Esc parks by writing nothing after the press,
            and the rest are declared, checked where they are read, and
            published for other software. */
        std::string panic = "park";

        /*  THE DECLARED SAFE VALUE, when `panic` is "value", and empty
            otherwise: one element per type tag, as a node's `values` is, so it
            is written out exactly as VALUE is - an array in OSCQuery's PANIC
            (namespace draft §3). */
        std::vector<osc::Value> panicValues;

        /*  THE SHAPE OF A NODE THAT SAYS NOTHING ABOUT ITSELF - a container's,
            and any node never given one: no sentence, no range, the GODOT
            key's defaults. One for the process. */
        static const NodeShape& none() noexcept
        {
            static const NodeShape nothing;
            return nothing;
        }
    };

    struct Node
    {
        /*  `Node::ArgumentRange` is the name its readers already use. Declared
            FIRST: a class that uses a name and then declares it as an alias
            changes what the name means inside the class, which GCC refuses
            (-Wchanges-meaning, an error in the strict job) and MSVC lets by. */
        using ArgumentRange = tree::ArgumentRange;

        /** The full OSC address. Unique, and the tree's only key. */
        std::string address;

        /*  The kind, the access and the type tags are the node's own rather
            than its shape's: two words of a byte each, and a list row's TYPE
            grows with the value it holds (`ddd` for three gains) - so the
            first two are no cheaper shared and the third is not shareable. */
        Kind kind = Kind::container;
        Access access = Access::none;

        /*  OSC type tags, one per argument: "s", "d", "T", or a command's whole
            signature like "sis". Empty for a container. */
        std::string typeTags;

        /*  What the node says about itself, shared with every node made from
            the same row or the same node of a file. Null says nothing, and is
            read through the accessors below as `NodeShape::none()`. */
        std::shared_ptr<const NodeShape> shape;

        //======================================================================
        /*  The value, when the node has one. EMPTY for a container, and empty
            for an event - which is the whole point of that distinction: asking
            an event for its value is a question with no answer, and a nil or a
            zero would be an answer.

            A VECTOR RATHER THAN AN OPTIONAL, and the reason is the protocol
            rather than Go.dot. OSCQuery's VALUE is an array and always has
            been - the JSON here has emitted `"VALUE": [x]` since Phase 1 - and
            OSC carries as many arguments as the type tag string declares. A
            node holding one value is the common case, not the only one:
            `Route/@gains` is C_in x width numbers under a single address, and
            modelling that as a string would have put a parser in every client.

            So `typeTags` and this stay the same length, and a node with one
            value has one of each. */
        std::vector<osc::Value> values;

        //======================================================================
        /*  What the node says about itself, through its shape: every reader
            asks these, and a node with no shape answers as a container does -
            no sentence, unbounded, the GODOT key's defaults. */
        const NodeShape& about() const noexcept { return shape != nullptr ? *shape : NodeShape::none(); }

        const std::string& description() const noexcept                { return about().description; }
        const std::string& role() const noexcept                       { return about().role; }
        bool open() const noexcept                                     { return about().open; }
        bool skips() const noexcept                                    { return about().skips; }
        const std::string& rcpVerb() const noexcept                    { return about().rcpVerb; }
        int rcpIndexes() const noexcept                                { return about().rcpIndexes; }
        const std::string& lineTemplate() const noexcept               { return about().lineTemplate; }
        const MidiShape& midi() const noexcept                         { return about().midi; }
        bool hasMinimum() const noexcept                               { return about().hasMinimum; }
        double minimum() const noexcept                                { return about().minimum; }
        bool hasMaximum() const noexcept                               { return about().hasMaximum; }
        double maximum() const noexcept                                { return about().maximum; }
        const std::vector<std::string>& enumValues() const noexcept    { return about().enumValues; }
        const std::vector<ArgumentRange>& laterRanges() const noexcept { return about().laterRanges; }
        const std::string& unit() const noexcept                       { return about().unit; }
        double rateCap() const noexcept                                { return about().rateCap; }
        bool anticipatable() const noexcept                            { return about().anticipatable; }
        const std::string& panic() const noexcept                      { return about().panic; }
        const std::vector<osc::Value>& panicValues() const noexcept    { return about().panicValues; }

        /*  The bounds of one argument, from nought: unbounded where none were
            given. */
        ArgumentRange rangeOf (std::size_t argument) const { return about().rangeOf (argument); }

        bool isContainer() const noexcept { return kind == Kind::container; }

        /*  The value, for a node that holds exactly one - which is almost all
            of them. Empty for a container, for an event, and for a LIST, and
            that last one is deliberate: a caller that meant "the value" and
            meets four gains has asked a question with no single answer, and
            handing back the first would be a wrong answer rather than none. */
        std::optional<osc::Value> soleValue() const
        {
            if (values.size() != 1)
                return std::nullopt;

            return values.front();
        }
    };
}
