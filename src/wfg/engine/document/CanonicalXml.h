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
    The show document on disk: canonical, editable, diffable XML (PRD §3.20).

    CANONICAL means one document has exactly one spelling, so that a save after
    a load is byte-identical and a diff shows what someone changed rather than
    what the writer felt like doing:

      * UTF-8, no byte-order mark
      * "\n" line endings on every platform
      * one element per line, two spaces of indent per level
      * attributes sorted by name, so reordering an edit cannot reorder a file
      * attributes at their default omitted, so a diff shows decisions
      * numbers in the shortest form that reads back identically
      * booleans as true / false
      * an element with no children written as <Cue …/>

    Not juce::XmlElement::writeTo: it emits attributes in insertion order, wraps
    long lines, and writes doubles through JUCE's formatter, which loses 46% of
    them to a round trip (measured; see osc/OscValue.cpp). Any one of those makes
    "load, save, compare" fail.

    READING is where the other half of the discipline lives. juce::ValueTree's
    own fromXml stores EVERY attribute as a string, so a document that has been
    loaded but not yet written to holds "12" where 12 belongs — WFS-DIY lists
    exactly that as a live defect. The reader here consults the schema for every
    attribute and builds a typed value, or refuses the file and says which
    attribute and why.
*/

#include <wfg/engine/document/ShowDocument.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace wfg::doc
{
    struct ReadResult
    {
        bool ok = false;

        /** One message per problem, in document order. Empty when ok. */
        std::vector<std::string> problems;

        static ReadResult failed (std::string problem)
        {
            ReadResult r;
            r.problems.push_back (std::move (problem));
            return r;
        }
    };

    namespace CanonicalXml
    {
        /** The document as canonical XML, ending in a newline. */
        std::string write (const ShowDocument& document);

        /*  Parses `text` into `document`, replacing whatever it held.

            Refuses rather than repairs. An unknown element, an unparseable
            number, a duplicate identifier — each is a problem in the result and
            the document is left untouched. A show that half-loaded is worse than
            one that did not: the operator would find out during the show. */
        ReadResult read (std::string_view text, ShowDocument& document);

        /** Convenience: read, then write, without keeping the document. Used by
            `wfg canon` and by the round-trip tests. */
        std::string canonicalise (std::string_view text, ReadResult& result);

        /*  One attribute of one node as canonical text, or nullopt when the
            node does not carry it or carries exactly its default - which is
            the rule that keeps both files sparse.

            Public because state.xml needs the identical rule and must not grow
            a second implementation of it. The two writers differ in WHICH
            attributes they take (`persist=show` here, `persist=state` there)
            and in the shape they arrange them in; how a value becomes text is
            the same question in both, and it is answered once. */
        std::optional<std::string> attributeText (const Attribute& attribute,
                                                  const juce::ValueTree& node);
    }
}
