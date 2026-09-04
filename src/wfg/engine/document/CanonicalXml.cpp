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

#include <wfg/engine/document/CanonicalXml.h>

#include <wfg/engine/osc/OscValue.h>

#include <juce_core/juce_core.h>

#include <algorithm>
#include <map>

namespace wfg::doc
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        /*  XML attribute escaping, and the list is deliberately longer than the
            minimum. `<` and `&` must be escaped; `"` must be inside a
            double-quoted value. `>` need not be, and the tab, newline and
            carriage return survive a parser without escaping but do not survive
            a ROUND TRIP: an XML reader normalises them to spaces in an
            attribute value, so a cue name with a tab in it would come back
            different. Escaping them is what makes the round trip exact. */
        std::string escapeAttribute (const std::string& text)
        {
            std::string out;
            out.reserve (text.size() + 8);

            for (const unsigned char c : text)
            {
                switch (c)
                {
                    case '&':  out += "&amp;";  break;
                    case '<':  out += "&lt;";   break;
                    case '>':  out += "&gt;";   break;
                    case '"':  out += "&quot;"; break;
                    case '\t': out += "&#9;";   break;
                    case '\n': out += "&#10;";  break;
                    case '\r': out += "&#13;";  break;
                    default:   out.push_back (static_cast<char> (c)); break;
                }
            }

            return out;
        }

        /*  The canonical text for one attribute, or nothing when it equals its
            default and can be left out.

            Omitting defaults is what keeps a diff about decisions: a cue that
            nobody disabled has no `enabled` attribute, so enabling and
            re-disabling one leaves the file exactly as it was. */
        std::optional<std::string> attributeText (const Attribute& attribute,
                                                  const juce::ValueTree& node)
        {
            const juce::Identifier property { juce::String (std::string (attribute.name())) };

            if (! node.hasProperty (property))
                return std::nullopt;

            const auto& raw = node[property];

            std::string text;

            switch (attribute.type())
            {
                case ValueType::string:
                    text = raw.toString().toStdString();
                    break;

                case ValueType::integer:
                case ValueType::integer64:
                    text = std::to_string (static_cast<long long> (raw));
                    break;

                case ValueType::boolean:
                    text = static_cast<bool> (raw) ? "true" : "false";
                    break;

                case ValueType::number:
                    text = osc::formatDouble (static_cast<double> (raw));
                    break;

                case ValueType::blob:
                    return std::nullopt;
            }

            if (attribute.hasDefault() && text == attribute.defaultText())
                return std::nullopt;

            return text;
        }

        void writeNode (const juce::ValueTree& node, int depth, std::string& out)
        {
            const auto elementName = node.getType().toString().toStdString();
            const auto* element = Schema::instance().element (elementName);

            const std::string indent (static_cast<std::size_t> (depth) * 2, ' ');

            out += indent;
            out += '<';
            out += elementName;

            /*  Sorted by name, with `id` first. Sorting means an edit cannot
                reorder a file; `id` first means the identity is where the eye
                lands, which matters when reading a cue list in a diff. */
            if (node.hasProperty (idProperty))
            {
                out += " id=\"";
                out += escapeAttribute (node[idProperty].toString().toStdString());
                out += '"';
            }

            if (element != nullptr)
            {
                std::map<std::string, std::string> sorted;

                for (const auto& attribute : element->attributes)
                {
                    if (auto text = attributeText (attribute, node))
                        sorted.emplace (std::string (attribute.name()), std::move (*text));
                }

                for (const auto& [name, value] : sorted)
                {
                    out += ' ';
                    out += name;
                    out += "=\"";
                    out += escapeAttribute (value);
                    out += '"';
                }
            }

            if (node.getNumChildren() == 0)
            {
                out += "/>\n";
                return;
            }

            out += ">\n";

            for (const auto& child : node)
                writeNode (child, depth + 1, out);

            out += indent;
            out += "</";
            out += elementName;
            out += ">\n";
        }
    }

    //==============================================================================
    std::string CanonicalXml::write (const ShowDocument& document)
    {
        /*  No XML declaration. It would carry an encoding the file already is
            (UTF-8 is XML's default) and a version nothing varies, and it is one
            more line to keep byte-identical for no reader's benefit. */
        std::string out;
        out.reserve (4096);
        writeNode (document.root(), 0, out);
        return out;
    }

    //==============================================================================
    namespace
    {
        /*  Builds a ValueTree from parsed XML with every value TYPED by the
            schema.

            Not ValueTree::fromXml, and this is the single most important line in
            the file: that function stores every attribute as a string
            (juce_NamedValueSet.cpp), so a freshly loaded document would hold
            "12" where 12 belongs, compare equal to a typed 12, and silently
            drop the first real write to it. WFS-DIY carries exactly that defect.

            Refuses rather than guesses: an unknown element or attribute, a value
            that does not parse, a duplicate identifier - each is a problem, and
            the caller throws the whole document away. */
        struct Builder
        {
            IdRegistry& registry;
            std::vector<std::string>& problems;

            juce::ValueTree build (const juce::XmlElement& xml, const std::string& path)
            {
                const auto elementName = xml.getTagName().toStdString();
                const auto* element = Schema::instance().element (elementName);

                std::string here = path + "/" + elementName;

                if (element == nullptr)
                {
                    problems.push_back (here + ": unknown element");
                    return {};
                }

                juce::ValueTree node { xml.getTagName() };

                if (xml.hasAttribute ("id"))
                {
                    const auto id = xml.getStringAttribute ("id").toStdString();
                    here += "[" + id + "]";

                    if (! element->hasIdentity)
                    {
                        problems.push_back (here + ": <" + elementName + "> may not carry an id");
                    }
                    else if (! Id::isValid (id))
                    {
                        problems.push_back (here + ": malformed id \"" + id + "\"");
                    }
                    else if (! registry.reserve (id))
                    {
                        problems.push_back (here + ": duplicate id \"" + id + "\"");
                    }
                    else
                    {
                        node.setProperty (idProperty, juce::String (id), nullptr);
                    }
                }
                else if (element->hasIdentity)
                {
                    problems.push_back (here + ": missing id");
                }

                for (int i = 0; i < xml.getNumAttributes(); ++i)
                {
                    const auto name = xml.getAttributeName (i).toStdString();

                    if (name == "id")
                        continue;

                    const auto* attribute = element->attribute (name);

                    if (attribute == nullptr)
                    {
                        problems.push_back (here + ": unknown attribute \"" + name + "\"");
                        continue;
                    }

                    Value value;
                    const auto text = xml.getAttributeValue (i).toStdString();
                    const auto parsed = Schema::parseValue (*attribute, text, value);

                    if (! parsed.ok)
                    {
                        problems.push_back (here + ": \"" + name + "\" " + parsed.error);
                        continue;
                    }

                    switch (value.type())
                    {
                        case ValueType::string:
                            node.setProperty (juce::Identifier (juce::String (name)),
                                              juce::var (juce::String (value.getString())), nullptr);
                            break;
                        case ValueType::integer:
                        case ValueType::integer64:
                            node.setProperty (juce::Identifier (juce::String (name)),
                                              juce::var (static_cast<juce::int64> (value.getInteger())), nullptr);
                            break;
                        case ValueType::number:
                            node.setProperty (juce::Identifier (juce::String (name)),
                                              juce::var (value.getNumber()), nullptr);
                            break;
                        case ValueType::boolean:
                            node.setProperty (juce::Identifier (juce::String (name)),
                                              juce::var (value.getBoolean()), nullptr);
                            break;
                        case ValueType::blob:
                            break;
                    }
                }

                for (auto* child : xml.getChildIterator())
                {
                    if (child->isTextElement())
                    {
                        /*  Whitespace between elements is the indentation; text
                            anywhere else is content this format does not have,
                            and dropping it silently would lose whatever someone
                            typed. */
                        if (child->getText().trim().isNotEmpty())
                            problems.push_back (here + ": unexpected text content");

                        continue;
                    }

                    if (! element->mayContain (child->getTagName().toStdString()))
                        problems.push_back (here + ": <" + elementName + "> may not contain <"
                                            + child->getTagName().toStdString() + ">");

                    if (auto built = build (*child, here); built.isValid())
                        node.addChild (built, -1, nullptr);
                }

                return node;
            }
        };
    }

    ReadResult CanonicalXml::read (std::string_view text, ShowDocument& document)
    {
        juce::XmlDocument parser { juce::String (std::string (text)) };
        const auto xml = parser.getDocumentElement();

        if (xml == nullptr)
            return ReadResult::failed ("not valid XML: "
                                       + parser.getLastParseError().toStdString());

        if (xml->getTagName() != juce::String (std::string (Schema::rootElement)))
            return ReadResult::failed ("the root element is <" + xml->getTagName().toStdString()
                                       + ">, expected <" + std::string (Schema::rootElement) + ">");

        ReadResult result;

        /*  Into a registry of its own, so a document that turns out to be
            broken leaves the one already loaded untouched. */
        auto registry = IdRegistry::withSystemEntropy();
        Builder builder { registry, result.problems };

        auto built = builder.build (*xml, "");

        if (! result.problems.empty() || ! built.isValid())
            return result;

        document.adopt (std::move (built), std::move (registry));

        /*  A last pass over the finished tree. The builder checks each element
            as it goes; this catches what only the whole document shows -
            and it is cheap insurance against the two disagreeing. */
        result.problems = document.validate();
        result.ok = result.problems.empty();
        return result;
    }

    std::string CanonicalXml::canonicalise (std::string_view text, ReadResult& result)
    {
        ShowDocument document;
        result = read (text, document);

        if (! result.ok)
            return {};

        return write (document);
    }
}
