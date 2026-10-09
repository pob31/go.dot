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
    A PATCH AS PURE DATA WRITES IT (namespace draft 51, ACM).

    A process cue keeps its patch as Pd's own text - what Pd saves in a .pd
    file - so that one made in Pd opens here and one made here opens in Pd.
    The text is a run of RECORDS, each ended by a semicolon Pd did not escape:

        #N canvas 0 0 450 300 12;
        #X obj 10 10 r /wfs/source/1/x;
        #X obj 10 40 * 2.5;
        #X connect 0 0 1 0;

    This file reads that text into what the canvas draws and the engine needs -
    boxes with their place and words, the lines between them, the names a
    patch sends to and hears - and writes it back. Every record keeps its own
    text as it was read, and writing gives an untouched record back byte for
    byte, so a patch made in Pd and only moved here differs from Pd's by the
    records that moved. A record changed here is written as Pd writes one: its
    words separated by single spaces, ended by ";" and a new line.

    Standard library only, and no clock: the client's model reads it too
    (namespace draft 14.16), and so do the tests, with nothing running.

    PD'S ESCAPES. A backslash keeps the next character from meaning anything:
    `\;` is a semicolon inside a message box rather than the end of a record,
    `\,` a comma, `\$1` a dollar sign, `\ ` a space inside one word. Words are
    kept as they were written, escapes included, and `unescaped` undoes them
    where a word is read for its meaning - a receive name, a box's text.
*/

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace wfg::process
{
    /*  One of Pd's atoms: a number or a word - what a patch hears and sends,
        and what the canvas's live boxes hand it (PC.8). */
    struct Atom
    {
        bool isNumber = true;
        double number = 0.0;
        std::string word;

        static Atom of (double value) { return { true, value, {} }; }
        static Atom of (std::string value) { return { false, 0.0, std::move (value) }; }

        bool operator== (const Atom& other) const
        {
            return isNumber == other.isNumber
                && (isNumber ? ! (number < other.number) && ! (other.number < number) : word == other.word);
        }
    };

    using Atoms = std::vector<Atom>;

    /*  One record: its text as read, from its first character up to and
        including the semicolon that ends it and the new line after it if there
        was one; and its words, escapes kept. A comma Pd did not escape is a
        word of its own (",") - it separates the messages of one record, as in
        "#X obj 10 10 metro 100, f 20". */
    struct PatchRecord
    {
        std::string raw;
        std::vector<std::string> words;
    };

    /*  What a box is, by the record that made it. */
    enum class BoxKind
    {
        object,     // #X obj
        message,    // #X msg
        number,     // #X floatatom
        symbol,     // #X symbolatom
        list,       // #X listbox
        comment,    // #X text
        subpatch,   // #N canvas ... #X restore - one box in the canvas around it
        other       // #X scalar and anything newer Pd writes as a box
    };

    /*  A box on a canvas, in the order Pd numbers them - which is what a line's
        "#X connect 0 0 1 0" counts in. */
    struct PatchBox
    {
        BoxKind kind = BoxKind::object;
        int x = 0;
        int y = 0;
        std::string text;     // its words after the place, joined by spaces, escapes kept
        int width = 0;        // ", f <n>" - characters - or 0 for as wide as its text
        std::size_t record = 0;   // the record that made it (a subpatch's: its #X restore)
        std::size_t canvas = 0;   // which canvas it sits on
    };

    /*  A line from one box's outlet to another's inlet, both on one canvas,
        numbered as Pd numbers that canvas's boxes. */
    struct PatchLine
    {
        std::size_t canvas = 0;
        int fromBox = 0;
        int outlet = 0;
        int toBox = 0;
        int inlet = 0;
        std::size_t record = 0;
    };

    /*  A canvas: the patch itself is canvas 0, a subpatch one more. */
    struct PatchCanvas
    {
        std::size_t record = 0;               // its #N canvas
        std::optional<std::size_t> parent;    // none for the patch itself
        std::size_t boxInParent = 0;          // its box in the parent, when it has one
        std::string name;                     // "pd <name>" of a subpatch; empty for the patch
    };

    struct Patch
    {
        std::vector<PatchRecord> records;
        std::vector<PatchCanvas> canvases;
        std::vector<PatchBox> boxes;
        std::vector<PatchLine> lines;

        /*  Text after the last record that ends no record - nothing, for a file
            Pd wrote - kept so that writing gives it back. */
        std::string trailing;

        /*  Why the text is not a patch, in one sentence, or empty. A patch with
            a problem still keeps every record, so it can be written back. */
        std::string problem;

        /*  The boxes of one canvas, in Pd's order: what a PatchLine's numbers
            count in. */
        std::vector<std::size_t> boxesOn (std::size_t canvas) const;
    };

    Patch parsePatch (std::string_view text);

    /*  The text again: each record's raw text, then `trailing`. */
    std::string writePatch (const Patch& patch);

    /*  A word with Pd's escapes undone. */
    std::string unescaped (std::string_view word);

    /*  A word as Pd writes it: a backslash before a space, a semicolon, a
        comma, a dollar sign or a backslash. */
    std::string escaped (std::string_view word);

    /*  The words of one record's text, as parsePatch splits them. */
    std::vector<std::string> splitWords (std::string_view recordText);

    /*  A record made from words: joined by single spaces, ";\n" after. */
    PatchRecord recordOf (std::vector<std::string> words);

    /*  The names a patch sends to and hears that Go.dot answers (ACG): every
        one starting with "/", and the catch-alls "out" and "in" - from
        [s]/[send] and [r]/[receive] boxes, the send and receive names of Pd's
        GUI boxes, and the receivers a message box's "\;" names. A name a patch
        builds as it runs ("$1") is not here: the catch-alls are how a patch
        reaches those. Each once, in the order first met. */
    struct PatchNames
    {
        std::vector<std::string> sends;
        std::vector<std::string> receives;
    };

    PatchNames namesIn (const Patch& patch);

    /*  The patch a new process cue starts with: an empty canvas with one
        comment, which is what Pd shows when the patch is opened. */
    std::string starterPatch();
}
