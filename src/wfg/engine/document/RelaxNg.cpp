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

#include <wfg/engine/document/RelaxNg.h>

#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/osc/OscValue.h>

#include <map>
#include <string>
#include <vector>

namespace wfg::doc
{
    namespace
    {
        /*  Every line goes through here, so the file has one indent rule and
            one line ending. LF on every platform, like the show document and
            for the same reason: a grammar that differs between platforms could
            not be committed and compared. */
        void line (std::string& out, int depth, const std::string& text)
        {
            out.append (static_cast<std::size_t> (depth) * 2, ' ');
            out += text;
            out += '\n';
        }

        /*  The five predefined entities are the whole job here. Everything this
            writes is either a name from the parameter table or a description
            someone typed into it; what those can contain is an ampersand or an
            angle bracket, and a grammar that is not well-formed XML is worse
            than no grammar at all. */
        std::string escape (std::string_view text)
        {
            std::string out;
            out.reserve (text.size() + 8);

            for (const char c : text)
            {
                switch (c)
                {
                    case '&':  out += "&amp;";  break;
                    case '<':  out += "&lt;";   break;
                    case '>':  out += "&gt;";   break;
                    case '"':  out += "&quot;"; break;
                    case '\'': out += "&apos;"; break;
                    default:   out += c;        break;
                }
            }

            return out;
        }

        /*  The attributes of one element in the persist class asked for, sorted
            by name. Sorted for the same reason the document writer sorts: a row
            moved in the CSV must not move a line in the grammar, or every such
            edit would show up as a diff in a generated file. */
        std::map<std::string, const Attribute*> attributesOf (const Element& element,
                                                              Persist wanted)
        {
            std::map<std::string, const Attribute*> sorted;

            for (const auto& attribute : element.attributes)
                if (attribute.persist() == wanted)
                    sorted.emplace (std::string (attribute.name()), &attribute);

            return sorted;
        }

        /*  The datatype half of an attribute: a closed set of values when the
            table declares one, an XSD datatype with the declared bounds
            otherwise.

            Booleans are a closed set rather than xsd:boolean deliberately.
            xsd:boolean also accepts `1` and `0`, and a canonical document has
            exactly one spelling for a value - so accepting the other spelling
            here would admit a file our own writer can never produce, and the
            byte-comparison after a round trip would then be the thing that
            failed. */
        void writeScalarDatatype (std::string& out, const Attribute& attribute, int depth);

        void writeDatatype (std::string& out, const Attribute& attribute, int depth)
        {
            /*  A LIST IS A WRAPPER, NOT A DATATYPE. RELAX NG's <list> splits the
                attribute on whitespace and matches the pattern inside against
                each token, so everything below - the bounds, the enum, the
                closed boolean set - applies per element with no second spelling
                of any of it. Zero values is a legal list: a cue routed nowhere
                yet has an empty `gains`, and the grammar has no business
                refusing a document the editor can produce. */
            if (attribute.isList())
            {
                line (out, depth, "<list>");
                line (out, depth + 1, "<zeroOrMore>");
                writeScalarDatatype (out, attribute, depth + 2);
                line (out, depth + 1, "</zeroOrMore>");
                line (out, depth, "</list>");
                return;
            }

            writeScalarDatatype (out, attribute, depth);
        }

        void writeScalarDatatype (std::string& out, const Attribute& attribute, int depth)
        {
            if (attribute.isEnum())
            {
                line (out, depth, "<choice>");

                for (std::size_t i = 0; i < attribute.row->numEnumValues; ++i)
                    line (out, depth + 1,
                          "<value>" + escape (attribute.row->enumValues[i]) + "</value>");

                line (out, depth, "</choice>");
                return;
            }

            if (attribute.type() == ValueType::boolean)
            {
                line (out, depth, "<choice>");
                line (out, depth + 1, "<value>true</value>");
                line (out, depth + 1, "<value>false</value>");
                line (out, depth, "</choice>");
                return;
            }

            std::string datatype = "string";
            bool numeric = false;

            switch (attribute.type())
            {
                case ValueType::string:    datatype = "string";       break;
                case ValueType::integer:   datatype = "integer";      numeric = true; break;
                case ValueType::integer64: datatype = "integer";      numeric = true; break;
                case ValueType::number:    datatype = "double";       numeric = true; break;
                case ValueType::blob:      datatype = "base64Binary"; break;

                case ValueType::boolean:
                    /*  Caught above and unreachable. Named anyway, because the
                        strict preset compiles with -Wswitch-enum, which asks for
                        every enumerator whether or not an earlier branch took
                        it. */
                    datatype = "boolean";
                    break;
            }

            if (! numeric || ! (attribute.row->hasMin || attribute.row->hasMax))
            {
                line (out, depth, "<data type=\"" + datatype + "\"/>");
                return;
            }

            line (out, depth, "<data type=\"" + datatype + "\">");

            /*  Bounds through the document's own number formatter, so the
                grammar spells a number the way a show file spells it. An
                integer bound goes through std::to_string instead: xsd:integer
                rejects "0.0" as a facet value, and formatDouble would write a
                whole number as "0" but is not the right tool for saying so. */
            const auto bound = [&attribute] (double value)
            {
                return attribute.type() == ValueType::number
                     ? osc::formatDouble (value)
                     : std::to_string (static_cast<long long> (value));
            };

            if (attribute.row->hasMin)
                line (out, depth + 1,
                      "<param name=\"minInclusive\">" + bound (attribute.row->minimum) + "</param>");

            if (attribute.row->hasMax)
                line (out, depth + 1,
                      "<param name=\"maxInclusive\">" + bound (attribute.row->maximum) + "</param>");

            line (out, depth, "</data>");
        }

        /*  One attribute. Everything except `id` is optional, and that is not
            laxness: the canonical writer omits an attribute holding its
            default, and an absent attribute reads back AS its default. An
            attribute made required here would reject every sparse document the
            engine writes. */
        void writeAttribute (std::string& out, const Attribute& attribute, int depth)
        {
            line (out, depth, "<optional>");
            line (out, depth + 1, "<attribute name=\"" + std::string (attribute.name()) + "\">");

            if (! attribute.description().empty())
                line (out, depth + 2,
                      "<a:documentation>" + escape (attribute.description()) + "</a:documentation>");

            writeDatatype (out, attribute, depth + 2);

            line (out, depth + 1, "</attribute>");
            line (out, depth, "</optional>");
        }

        void writeIdAttribute (std::string& out, int depth)
        {
            line (out, depth, "<attribute name=\"id\">");
            line (out, depth + 1, "<ref name=\"identifier\"/>");
            line (out, depth, "</attribute>");
        }

        /*  A show element: its identity, its `persist=show` attributes and its
            children.

            The child rule is read out of the containment table rather than
            written down a second time. A child that carries an identity is an
            OBJECT and repeats - a list holds any number of cues; a child that
            does not is a CONTAINER, appears exactly once, and in the order the
            table lists it. That is the entire difference between <Lists> and
            <List>, and it is why neither has to be special-cased here. */
        void writeShowElement (std::string& out, const Element& element, const Schema& schema)
        {
            const auto name = std::string (element.name);
            const auto attributes = attributesOf (element, Persist::show);

            line (out, 1, "<define name=\"" + name + "\">");
            line (out, 2, "<element name=\"" + name + "\">");

            if (element.hasIdentity)
                writeIdAttribute (out, 3);

            for (const auto& entry : attributes)
                writeAttribute (out, *entry.second, 3);

            std::vector<std::string> objects, containers;

            for (const auto& child : element.childElements)
            {
                const auto* childElement = schema.element (child);

                if (childElement != nullptr && childElement->hasIdentity)
                    objects.push_back (std::string (child));
                else
                    containers.push_back (std::string (child));
            }

            for (const auto& container : containers)
                line (out, 3, "<ref name=\"" + container + "\"/>");

            if (! objects.empty())
            {
                line (out, 3, "<zeroOrMore>");

                if (objects.size() == 1)
                {
                    line (out, 4, "<ref name=\"" + objects.front() + "\"/>");
                }
                else
                {
                    line (out, 4, "<choice>");

                    for (const auto& object : objects)
                        line (out, 5, "<ref name=\"" + object + "\"/>");

                    line (out, 4, "</choice>");
                }

                line (out, 3, "</zeroOrMore>");
            }

            /*  RELAX NG cannot say "nothing here" by leaving it out: a pattern
                has to be present, and <empty/> is the one that matches nothing.
                No element in the table needs it today; it is here so that
                adding a bare marker element to the CSV produces a valid grammar
                rather than one that silently rejects the element it describes. */
            if (! element.hasIdentity && containers.empty() && objects.empty()
                && attributes.empty())
                line (out, 3, "<empty/>");

            line (out, 2, "</element>");
            line (out, 1, "</define>");
            out += '\n';
        }

        /*  An element as state.xml carries it: the identity, and only the
            attributes whose persist class is `state`. */
        void writeStateElement (std::string& out, const Element& element)
        {
            const auto name = std::string (element.name);

            line (out, 1, "<define name=\"State." + name + "\">");
            line (out, 2, "<element name=\"" + name + "\">");

            /*  A container entry carries no identifier, because there is one of
                it. `<Lists focus="...">` is the collection saying something
                about itself; `<List id="..." standby="...">` is one member. */
            if (element.hasIdentity)
                writeIdAttribute (out, 3);

            for (const auto& entry : attributesOf (element, Persist::state))
                writeAttribute (out, *entry.second, 3);

            line (out, 2, "</element>");
            line (out, 1, "</define>");
            out += '\n';
        }
    }

    //==============================================================================
    std::string RelaxNg::generate()
    {
        const auto& schema = Schema::instance();

        std::string out;
        out.reserve (16384);

        out += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        out += "<!--\n"
               "  The grammar of a Go.dot bundle: show.xml, state.xml and the .wfg manifest.\n"
               "\n"
               "  GENERATED. `wfg schema` writes this from the engine's schema table, which is\n"
               "  itself generated from docs/parameters/godot-parameters.csv. A test fails when\n"
               "  this committed copy and the generated one differ, so editing it by hand only\n"
               "  produces a red build. Add the parameter to the CSV instead.\n"
               "\n"
               "  Everything except `id` is optional, because the canonical writer omits an\n"
               "  attribute holding its default and an absent attribute reads back AS its\n"
               "  default. The grammar says which values are legal, not which are present.\n"
               "-->\n";
        out += "<grammar xmlns=\"http://relaxng.org/ns/structure/1.0\"\n"
               "         xmlns:a=\"http://relaxng.org/ns/compatibility/annotations/1.0\"\n"
               "         datatypeLibrary=\"http://www.w3.org/2001/XMLSchema-datatypes\">\n";
        out += '\n';

        line (out, 1, "<start>");
        line (out, 2, "<choice>");
        line (out, 3, "<ref name=\"Show\"/>");
        line (out, 3, "<ref name=\"State\"/>");
        line (out, 3, "<ref name=\"Bundle\"/>");
        line (out, 2, "</choice>");
        line (out, 1, "</start>");
        out += '\n';

        line (out, 1, "<define name=\"identifier\">");
        line (out, 2, "<data type=\"string\">");
        line (out, 3, "<param name=\"pattern\">" + std::string (Id::pattern) + "</param>");
        line (out, 2, "</data>");
        line (out, 1, "</define>");
        out += '\n';

        for (const auto& element : schema.elements())
            writeShowElement (out, element, schema);

        //======================================================================
        /*  state.xml. Flat, rather than a second copy of the show's shape: one
            entry per object that has something ephemeral to remember, found by
            identifier. A nested mirror would have to carry every container and
            every object that had nothing to say. */
        std::vector<const Element*> stateful;

        /*  IDENTIFIED OBJECTS AND CONTAINERS BOTH. An entry is either a `<List
            id=... standby=...>` - one object, found by identifier - or a
            `<Lists focus=...>`, which is the collection saying something about
            itself and has no identifier because there is only one of it.

            The grammar tells them apart exactly as the file does: whether the
            entry carries an id. */
        for (const auto& element : schema.elements())
        {
            if (attributesOf (element, Persist::state).empty())
                continue;

            if (element.hasIdentity
                  || ! ShowDocument::containerSegmentFor (element.name).empty())
                stateful.push_back (&element);
        }

        line (out, 1, "<define name=\"State\">");
        line (out, 2, "<element name=\"State\">");
        line (out, 3, "<attribute name=\"formatVersion\">");
        line (out, 4, "<data type=\"integer\"/>");
        line (out, 3, "</attribute>");

        if (stateful.empty())
        {
            line (out, 3, "<empty/>");
        }
        else
        {
            line (out, 3, "<zeroOrMore>");

            if (stateful.size() == 1)
            {
                line (out, 4, "<ref name=\"State." + std::string (stateful.front()->name) + "\"/>");
            }
            else
            {
                line (out, 4, "<choice>");

                for (const auto* element : stateful)
                    line (out, 5, "<ref name=\"State." + std::string (element->name) + "\"/>");

                line (out, 4, "</choice>");
            }

            line (out, 3, "</zeroOrMore>");
        }

        line (out, 2, "</element>");
        line (out, 1, "</define>");
        out += '\n';

        for (const auto* element : stateful)
            writeStateElement (out, *element);

        //======================================================================
        /*  The manifest: the file someone double-clicks. It carries a version
            and nothing else, on purpose - anything more in it would be a second
            place to look for something show.xml already says, and the two would
            eventually disagree. */
        line (out, 1, "<define name=\"Bundle\">");
        line (out, 2, "<element name=\"Bundle\">");
        line (out, 3, "<attribute name=\"formatVersion\">");
        line (out, 4, "<data type=\"integer\"/>");
        line (out, 3, "</attribute>");
        line (out, 2, "</element>");
        line (out, 1, "</define>");
        out += '\n';

        out += "</grammar>\n";

        return out;
    }
}
