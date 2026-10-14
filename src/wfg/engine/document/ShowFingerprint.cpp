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

#include <wfg/engine/document/ShowFingerprint.h>

#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/ShowDocument.h>
#include <wfg/engine/video/Movie.h>

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>

namespace wfg::doc
{
    namespace
    {
        const juce::Identifier idProperty { "id" };
        const juce::Identifier nameProperty { "name" };
        const juce::Identifier numberProperty { "number" };
        const juce::Identifier fileProperty { "file" };

        /*  WHAT THE HASHES BEGIN WITH: a word for what they are and a number
            for how they are made, so a later way of making them is a different
            fingerprint and never a match by accident. */
        constexpr const char* fingerprintFormat = "go.dot show fingerprint 1\n";
        constexpr const char* mediaFormat = "go.dot media list 1\n";

        std::string hexOf (const void* data, std::size_t size)
        {
            return juce::SHA256 (data, size).toHexString().toStdString();
        }

        std::string hexOf (const std::string& text)
        {
            return hexOf (text.data(), text.size());
        }

        std::string idOf (const juce::ValueTree& node)
        {
            return node[idProperty].toString().toStdString();
        }

        bool isCue (const juce::ValueTree& node)
        {
            return ShowDocument::ownerForElement (node.getType().toString().toStdString()) == "cue";
        }

        /*  HOW AN OPERATOR READS A UNIT, as the window and the logs name them:
            a cue by its number and name, a list by its name, a device - a
            mount - as a device, anything else with an identity by its owner
            word and its name, and a container by its element. */
        std::string labelOf (const juce::ValueTree& node, bool rootChild)
        {
            const auto element = node.getType().toString().toStdString();
            const auto id = idOf (node);
            const auto name = node[nameProperty].toString().toStdString();

            if (isCue (node))
            {
                const auto number = node[numberProperty].toString().toStdString();

                if (! number.empty() && ! name.empty())
                    return number + " " + name;

                if (! number.empty())
                    return number;

                return name.empty() ? id : name;
            }

            if (id.empty() || rootChild)
                return element;

            const auto shown = name.empty() ? id : name;

            if (element == "List")
                return "list " + shown;

            if (element == "Mount")
                return "device " + shown;

            const auto owner = std::string (ShowDocument::ownerForElement (element));
            return (owner.empty() ? element : owner) + " " + shown;
        }

        //======================================================================
        /*  THE WALK THAT CUTS A SHOW INTO UNITS (ShowFingerprint.h). A node is
            a unit of its own when it is the root, a container of the root, a
            cue, or an object with an identity that is not inside a cue; any
            other node is part of the unit it is in - written into that unit's
            text where it stands, as canonical XML writes it - and a unit inside
            another is written there only as a place in its order. */
        struct Cutter
        {
            std::vector<ShowUnit>& units;

            void unit (const juce::ValueTree& node, std::string key, bool rootChild)
            {
                ShowUnit made;
                made.key = std::move (key);
                made.element = node.getType().toString().toStdString();
                made.label = labelOf (node, rootChild);

                //  Its place in the list now; filled when its children have been cut.
                const auto at = units.size();
                units.push_back (made);

                std::string text, order;
                write (node, 0, isCue (node), node.getParent().isValid() == false, std::string(), text, order);

                units[at].hash = hexOf (text);
                units[at].order = std::move (order);
            }

            void write (const juce::ValueTree& node, int depth, bool insideCue, bool isRoot,
                        const std::string& path, std::string& text, std::string& order)
            {
                text.append (static_cast<std::size_t> (depth) * 2, ' ');
                text += CanonicalXml::openingOf (node, false);

                if (node.getNumChildren() == 0)
                {
                    text += "/>\n";
                    return;
                }

                text += ">\n";

                for (const auto& child : node)
                {
                    const auto childId = idOf (child);
                    const auto childIsUnit = isRoot || isCue (child) || (! childId.empty() && ! insideCue);

                    if (childIsUnit)
                    {
                        auto key = ! childId.empty() ? childId : child.getType().toString().toStdString();

                        if (! order.empty())
                            order += ' ';

                        order += path + key;
                        unit (child, std::move (key), isRoot);
                        continue;
                    }

                    write (child, depth + 1, insideCue, false,
                           path + child.getType().toString().toStdString() + "/", text, order);
                }

                text.append (static_cast<std::size_t> (depth) * 2, ' ');
                text += "</";
                text += node.getType().toString().toStdString();
                text += ">\n";
            }
        };

        std::int64_t secondsOf (std::int64_t milliseconds)
        {
            return milliseconds >= 0 ? milliseconds / 1000 : -((-milliseconds + 999) / 1000);
        }
    }

    //==========================================================================
    std::string fingerprintOf (std::string_view decided, const std::vector<Description>& descriptions)
    {
        juce::MemoryOutputStream combined;
        combined.write (fingerprintFormat, std::char_traits<char>::length (fingerprintFormat));
        combined.writeInt64 (static_cast<juce::int64> (decided.size()));
        combined.write (decided.data(), decided.size());

        for (const auto& description : descriptions)
        {
            combined.writeString (juce::String (description.name));
            combined.writeInt64 (static_cast<juce::int64> (description.bytes.size()));
            combined.write (description.bytes.data(), description.bytes.size());
        }

        return hexOf (combined.getData(), combined.getDataSize());
    }

    std::vector<Description> descriptionsIn (const juce::File& bundle)
    {
        std::vector<Description> found;

        if (bundle == juce::File() || ! bundle.isDirectory())
            return found;

        const auto folder = bundle.getChildFile ("namespaces");

        if (! folder.isDirectory())
            return found;

        for (const auto& file : folder.findChildFiles (juce::File::findFiles, false))
        {
            juce::MemoryBlock bytes;

            /*  A FILE THAT WILL NOT READ is kept, empty: it is still a file the
                other machine may have, and a description nobody can read here
                is a difference worth saying. */
            if (! file.loadFileAsData (bytes))
                bytes.reset();

            Description description;
            description.name = file.getFileName().toStdString();

            if (bytes.getSize() > 0)
                description.bytes.assign (static_cast<const char*> (bytes.getData()), bytes.getSize());

            found.push_back (std::move (description));
        }

        //  By name, so two machines listing the same folder in their own order agree.
        std::sort (found.begin(), found.end(),
                   [] (const Description& a, const Description& b) { return a.name < b.name; });

        return found;
    }

    std::vector<ShowUnit> showUnitsOf (const juce::ValueTree& show, const std::vector<Description>& descriptions)
    {
        std::vector<ShowUnit> units;

        if (show.isValid())
        {
            Cutter cutter { units };
            cutter.unit (show, "Show", false);
        }

        for (const auto& description : descriptions)
        {
            ShowUnit unit;
            unit.key = "namespaces/" + description.name;
            unit.element = "Description";
            unit.hash = hexOf (description.bytes);
            unit.label = description.name;
            units.push_back (std::move (unit));
        }

        return units;
    }

    std::vector<MediaEntry> mediaListOf (const juce::ValueTree& show, const std::string& mediaFolder,
                                         const audio::MediaRecords* analysed)
    {
        std::map<std::string, std::string> named;

        /*  The `file` of every media cue and every video cue: the walk
            `audio::mediaFilesNamedBy` and `pictureFilesNamedBy` make of a
            document, made here of a tree, since a copy handed to another
            thread has no document around it. */
        const std::function<void (const juce::ValueTree&)> visit = [&] (const juce::ValueTree& node)
        {
            const auto element = node.getType().toString();

            if (element == "Media" || element == "Video")
            {
                const auto file = node[fileProperty].toString().toStdString();

                if (! file.empty() && named.count (file) == 0)
                    named.emplace (file, element == "Media" ? "sound"
                                        : video::movie::isMovieName (file) ? "movie" : "picture");
            }

            for (const auto& child : node)
                visit (child);
        };

        if (show.isValid())
            visit (show);

        std::vector<MediaEntry> entries;
        entries.reserve (named.size());

        for (const auto& [path, kind] : named)
        {
            MediaEntry entry;
            entry.path = path;
            entry.kind = kind;

            /*  WHERE THE SHOW WOULD LOOK FOR IT on this machine - its own
                media/, then the one around it - and nothing when that is not a
                full path: a show that is not a bundle finds no file, and a
                relative File is an assertion in a JUCE debug build. */
            const auto resolved = audio::resolveMediaPath (mediaFolder, path);

            if (juce::File::isAbsolutePath (juce::String (resolved)))
            {
                const juce::File file { juce::String (resolved) };

                if (file.existsAsFile())
                {
                    entry.present = true;
                    entry.size = file.getSize();
                    entry.modified = file.getLastModificationTime().toMilliseconds();
                }
            }

            if (analysed != nullptr && entry.present)
                if (const auto found = analysed->find (path); found != analysed->end())
                    entry.hash = found->second.contentHash;

            entries.push_back (std::move (entry));
        }

        return entries;
    }

    std::string mediaDigestOf (const std::vector<MediaEntry>& media)
    {
        std::string text = mediaFormat;

        for (const auto& entry : media)
        {
            text += entry.path;
            text += '\t';
            text += entry.kind;
            text += entry.present ? "\tthere\t" : "\tmissing\t";
            text += std::to_string (entry.size);
            text += '\t';

            //  The bytes when they are known; when they are not, when the file was last written, to the second.
            text += entry.hash.empty() ? "written " + std::to_string (secondsOf (entry.modified)) : entry.hash;
            text += '\n';
        }

        return hexOf (text);
    }

    ShowFingerprint fingerprintShow (const juce::ValueTree& show, std::uint64_t revision,
                                     const juce::File& bundle, const std::string& mediaFolder,
                                     const audio::MediaRecords* analysed)
    {
        ShowFingerprint print;
        print.revision = revision;
        print.decided = show.isValid() ? CanonicalXml::writeDecided (show) : std::string();
        print.descriptions = descriptionsIn (bundle);
        print.fingerprint = fingerprintOf (print.decided, print.descriptions);
        print.units = showUnitsOf (show, print.descriptions);
        print.media = mediaListOf (show, mediaFolder, analysed);
        print.mediaDigest = mediaDigestOf (print.media);
        return print;
    }

    //==========================================================================
    void putMachineRowsBack (juce::ValueTree taken, const juce::ValueTree& own)
    {
        std::map<std::string, juce::ValueTree> ownById;

        const std::function<void (const juce::ValueTree&)> index = [&] (const juce::ValueTree& node)
        {
            if (const auto id = idOf (node); ! id.empty())
                ownById.emplace (id, node);

            for (const auto& child : node)
                index (child);
        };

        if (own.isValid())
            index (own);

        const std::function<void (juce::ValueTree, const juce::ValueTree&)> visit =
            [&] (juce::ValueTree node, const juce::ValueTree& mine)
        {
            if (const auto* element = Schema::instance().element (node.getType().toString().toStdString()))
            {
                for (const auto& attribute : element->attributes)
                {
                    if (! attribute.isMachine())
                        continue;

                    const juce::Identifier property { juce::String (std::string (attribute.name())) };

                    if (mine.isValid() && mine.hasProperty (property))
                        node.setProperty (property, mine[property], nullptr);
                    else
                        node.removeProperty (property, nullptr);
                }
            }

            /*  EACH CHILD'S OWN: the node of this machine's show with the same
                identifier, of the same element; for one with none, the node of
                the same element at the same place among its kind under this
                node's own. */
            std::map<juce::String, int> seen;

            for (auto child : node)
            {
                const auto type = child.getType().toString();
                const auto place = seen[type]++;
                juce::ValueTree counterpart;

                if (const auto id = idOf (child); ! id.empty())
                {
                    if (const auto found = ownById.find (id);
                        found != ownById.end() && found->second.getType() == child.getType())
                        counterpart = found->second;
                }
                else if (mine.isValid())
                {
                    int count = 0;

                    for (const auto& candidate : mine)
                        if (candidate.getType() == child.getType() && count++ == place)
                        {
                            counterpart = candidate;
                            break;
                        }
                }

                visit (child, counterpart);
            }
        };

        if (taken.isValid())
            visit (taken, own);
    }
}
