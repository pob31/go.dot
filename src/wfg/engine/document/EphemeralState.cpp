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

#include <wfg/engine/document/EphemeralState.h>

#include <wfg/engine/document/Ids.h>
#include <wfg/engine/document/Schema.h>

#include <juce_core/juce_core.h>

#include <map>
#include <string>

namespace wfg::doc
{
    namespace
    {
        const juce::Identifier idProperty { "id" };

        constexpr std::string_view rootElement = "State";

        /*  The same five entities the show writer escapes, and deliberately a
            second small copy rather than a shared one: this file and the show
            writer are allowed to diverge (a future state.xml could hold text
            content, which show.xml never will), and a helper shared between
            two writers that must not drift is a helper that hides it when they
            do. Nine lines is a cheap price for that independence. */
        std::string escapeAttribute (const std::string& text)
        {
            std::string out;
            out.reserve (text.size() + 8);

            for (const char raw : text)
            {
                const auto c = static_cast<unsigned char> (raw);

                switch (c)
                {
                    case '&':  out += "&amp;";  break;
                    case '<':  out += "&lt;";   break;
                    case '>':  out += "&gt;";   break;
                    case '"':  out += "&quot;"; break;
                    case '\'': out += "&apos;"; break;
                    default:   out += static_cast<char> (c); break;
                }
            }

            return out;
        }

        /*  Depth-first in document order, so state.xml lists its entries in the
            order the show lists the objects they belong to. Nothing reads them
            in order; a person diffing two of them does. */
        void collect (const juce::ValueTree& node, std::string& out)
        {
            const auto elementName = node.getType().toString().toStdString();
            const auto* element = Schema::instance().element (elementName);

            if (element != nullptr && node.hasProperty (idProperty))
            {
                std::map<std::string, std::string> sorted;

                for (const auto& attribute : element->attributes)
                {
                    if (attribute.persist() != Persist::state)
                        continue;

                    if (auto text = CanonicalXml::attributeText (attribute, node))
                        sorted.emplace (std::string (attribute.name()), std::move (*text));
                }

                /*  An object with nothing to remember is left out entirely,
                    which is what keeps this file short enough to read: a show
                    with four hundred cues and one standby is two lines. */
                if (! sorted.empty())
                {
                    out += "  <" + elementName;
                    out += " id=\"" + escapeAttribute (node[idProperty].toString().toStdString()) + "\"";

                    for (const auto& [name, value] : sorted)
                        out += " " + name + "=\"" + escapeAttribute (value) + "\"";

                    out += "/>\n";
                }
            }

            for (const auto& child : node)
                collect (child, out);
        }
    }

    //==============================================================================
    std::string EphemeralState::write (const ShowDocument& document)
    {
        std::string out;

        /*  formatVersion is written even though it is the default, and that is
            the one place this file departs from the sparse rule. show.xml can
            omit it because the schema supplies the default for an absent
            attribute; <State> has no schema row behind it, so the number has to
            be in the file or a future reader would have nothing to check. */
        out += "<" + std::string (rootElement) + " formatVersion=\""
             + std::to_string (Schema::formatVersion()) + "\"";

        std::string entries;
        collect (document.root(), entries);

        if (entries.empty())
        {
            out += "/>\n";
            return out;
        }

        out += ">\n";
        out += entries;
        out += "</" + std::string (rootElement) + ">\n";

        return out;
    }

    //==============================================================================
    ReadResult EphemeralState::read (std::string_view text, ShowDocument& document)
    {
        ReadResult result;

        juce::XmlDocument parser { juce::String (std::string (text)) };
        const auto xml = parser.getDocumentElement();

        if (xml == nullptr)
            return ReadResult::failed ("state.xml is not valid XML: "
                                       + parser.getLastParseError().toStdString());

        if (xml->getTagName() != juce::String (std::string (rootElement)))
            return ReadResult::failed ("state.xml's root element is <"
                                       + xml->getTagName().toStdString() + ">, expected <"
                                       + std::string (rootElement) + ">");

        /*  A version from the future is the one thing here that is fatal.
            Everything else in this file can be skipped and leave the show
            usable; a newer format may mean an entry that LOOKS readable and is
            not, and quietly applying half of it is how a standby ends up
            pointing at the wrong cue. */
        const auto version = xml->getIntAttribute ("formatVersion", 0);

        if (version > Schema::formatVersion())
            return ReadResult::failed ("state.xml is format version "
                                       + std::to_string (version) + "; this build understands "
                                       + std::to_string (Schema::formatVersion()));

        const auto& schema = Schema::instance();

        for (auto* entry : xml->getChildIterator())
        {
            if (entry->isTextElement())
                continue;

            const auto elementName = entry->getTagName().toStdString();
            const auto* element = schema.element (elementName);

            if (element == nullptr || ! element->hasIdentity)
            {
                result.problems.push_back ("state.xml: <" + elementName
                                           + "> is not an identified show element");
                continue;
            }

            const auto id = entry->getStringAttribute ("id").toStdString();

            if (! Id::isValid (id))
            {
                result.problems.push_back ("state.xml: <" + elementName
                                           + "> has no usable id");
                continue;
            }

            /*  The object may simply be gone - deleted since the state was
                written, or the state carried over from a different show. That
                is a stale pointer, not a broken document: say so and move on. */
            const auto node = document.findById (id);

            if (! node.isValid())
            {
                result.problems.push_back ("state.xml: no object " + id
                                           + " in this show; entry skipped");
                continue;
            }

            const auto owner = ShowDocument::ownerForElement (node.getType().toString().toStdString());

            for (int i = 0; i < entry->getNumAttributes(); ++i)
            {
                const auto name = entry->getAttributeName (i).toStdString();

                if (name == "id" || name == "formatVersion")
                    continue;

                const auto* attribute = element->attribute (name);

                if (attribute == nullptr || attribute->persist() != Persist::state)
                {
                    result.problems.push_back ("state.xml: <" + elementName + "> has no ephemeral"
                                               " attribute \"" + name + "\"");
                    continue;
                }

                /*  Through ShowDocument's one write path, so a value restored
                    from a file is checked exactly as a value written over OSC
                    would be. A read-only ephemeral attribute would be refused
                    here, which is the right noise to make: nothing should be
                    able to save something no command can set. */
                const auto address = "/godot/" + std::string (owner) + "/" + id + "/" + name;
                const auto edit = document.setAttribute (address,
                                                         entry->getAttributeValue (i).toStdString());

                if (! edit.ok)
                    result.problems.push_back ("state.xml: could not restore " + address
                                               + " (" + edit.reason + ")");
            }
        }

        result.ok = result.problems.empty();
        return result;
    }
}
