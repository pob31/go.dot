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

#include <wfg/engine/document/Template.h>

#include <wfg/engine/app/OpenShows.h>
#include <wfg/engine/document/Bundle.h>
#include <wfg/engine/document/CanonicalXml.h>
#include <wfg/engine/document/Schema.h>
#include <wfg/engine/document/ShowDocument.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>

namespace wfg::doc::Template
{
    namespace
    {
        const juce::Identifier idKey { "id" };

        std::string idOf (const juce::ValueTree& node)
        {
            return node.getProperty (idKey).toString().toStdString();
        }

        std::string tagOf (const juce::ValueTree& node)
        {
            return node.getType().toString().toStdString();
        }

        bool isCue (const juce::ValueTree& node)
        {
            return ShowDocument::ownerForElement (tagOf (node)) == "cue";
        }

        /*  WHAT THE LIST IS MADE OF: lists, the sections inside groups, and
            cues. Everything else under Lists - a route, a feed, a range, a
            trigger - is PART of the cue it sits in, and compared as one of
            that cue's fields. */
        bool isItem (const juce::ValueTree& node)
        {
            const auto tag = tagOf (node);
            return isCue (node) || tag == "List" || tag == "Header" || tag == "Footer" || tag == "Persistent";
        }

        //  The node with `id` anywhere under `node`, or an invalid one.
        juce::ValueTree findIn (const juce::ValueTree& node, const std::string& id)
        {
            if (id.empty())
                return {};

            for (int i = 0; i < node.getNumChildren(); ++i)
            {
                const auto child = node.getChild (i);

                if (idOf (child) == id)
                    return child;

                if (auto found = findIn (child, id); found.isValid())
                    return found;
            }

            return {};
        }

        //  Every item under `node`, depth first, in document order.
        void itemsUnder (const juce::ValueTree& node, std::vector<juce::ValueTree>& out)
        {
            for (int i = 0; i < node.getNumChildren(); ++i)
            {
                const auto child = node.getChild (i);

                if (isItem (child) && ! idOf (child).empty())
                {
                    out.push_back (child);
                    itemsUnder (child, out);
                }
            }
        }

        //  The item before `node` among its parent's items, or invalid when it is the first.
        juce::ValueTree previousItem (const juce::ValueTree& node)
        {
            const auto parent = node.getParent();

            for (int i = parent.indexOf (node) - 1; i >= 0; --i)
                if (isItem (parent.getChild (i)))
                    return parent.getChild (i);

            return {};
        }

        std::string text (const juce::ValueTree& node, const char* property)
        {
            return node.getProperty (property).toString().toStdString();
        }

        std::string labelOf (const juce::ValueTree& node)
        {
            const auto tag = tagOf (node);

            if (tag == "List")
                return "the list " + (text (node, "name").empty() ? idOf (node) : text (node, "name"));

            if (tag == "Header" || tag == "Footer" || tag == "Persistent")
            {
                const auto word = tag == "Header" ? "the header" : tag == "Footer" ? "the footer" : "the persistent cues";
                return std::string (word) + " of " + labelOf (node.getParent());
            }

            const auto number = text (node, "number");
            const auto name = text (node, "name");
            const auto said = (number.empty() ? std::string() : number + " ") + (name.empty() ? tag : name);

            return said.empty() ? idOf (node) : said;
        }

        std::string fieldLabel (const std::string& field)
        {
            static const std::map<std::string, std::string> words {
                { "position", "place in the list" },
                { "child:Route", "routing" }, { "child:Feed", "feeds" }, { "child:Insert", "inserts" },
                { "child:Range", "ranges" }, { "child:Trigger", "triggers" },
            };

            if (const auto known = words.find (field); known != words.end())
                return known->second;

            if (field.rfind ("child:", 0) == 0)
                return juce::String (field.substr (6)).toLowerCase().toStdString();

            return field;
        }

        //  A node, or every child of one kind, as canonical XML: how two are told apart.
        std::string canonical (const std::vector<juce::ValueTree>& nodes)
        {
            return CanonicalXml::writeFragment (nodes);
        }

        std::vector<juce::ValueTree> partsTagged (const juce::ValueTree& node, const std::string& tag)
        {
            std::vector<juce::ValueTree> parts;

            for (int i = 0; i < node.getNumChildren(); ++i)
                if (tagOf (node.getChild (i)) == tag)
                    parts.push_back (node.getChild (i));

            return parts;
        }

        std::set<std::string> partTags (const juce::ValueTree& node)
        {
            std::set<std::string> tags;

            for (int i = 0; i < node.getNumChildren(); ++i)
                if (! isItem (node.getChild (i)))
                    tags.insert (tagOf (node.getChild (i)));

            return tags;
        }

        /*  Where an item sits: its parent's id (the list's for a list), then
            the item before it AMONG THOSE BOTH DOCUMENTS HAVE - so a cue added
            or deleted beside it does not make it look moved. */
        std::string placeOf (const juce::ValueTree& node, const std::set<std::string>& common)
        {
            const auto parent = node.getParent();
            std::string before;

            for (int i = parent.indexOf (node) - 1; i >= 0 && before.empty(); --i)
                if (isItem (parent.getChild (i)) && common.count (idOf (parent.getChild (i))) > 0)
                    before = idOf (parent.getChild (i));

            return (tagOf (parent) == "Lists" ? std::string ("Lists") : idOf (parent)) + "/" + before;
        }

        //  An attribute as the file would write it: absent when it holds its default.
        std::optional<std::string> spelled (const juce::ValueTree& node, const juce::Identifier& name)
        {
            if (const auto* attribute = Schema::instance().attribute (tagOf (node), name.toString().toStdString()))
                return CanonicalXml::attributeText (*attribute, node);

            if (! node.hasProperty (name))
                return std::nullopt;

            return node.getProperty (name).toString().toStdString();
        }

        std::vector<TemplateField> fieldsThatDiffer (const juce::ValueTree& p, const juce::ValueTree& t,
                                                     const std::set<std::string>& common)
        {
            std::vector<TemplateField> fields;
            std::set<juce::Identifier> names;

            for (int i = 0; i < p.getNumProperties(); ++i) names.insert (p.getPropertyName (i));
            for (int i = 0; i < t.getNumProperties(); ++i) names.insert (t.getPropertyName (i));

            for (const auto& name : names)
            {
                if (name == idKey)
                    continue;

                /*  AS THE FILE WOULD SAY IT: an attribute holding its default
                    is written as absent, so "timeout=5" spelled out and left
                    out are the same cue (CanonicalXml.h, `attributeText`). */
                if (spelled (p, name) != spelled (t, name))
                {
                    const auto field = name.toString().toStdString();
                    fields.push_back ({ field, fieldLabel (field) });
                }
            }

            auto tags = partTags (p);
            const auto theirs = partTags (t);
            tags.insert (theirs.begin(), theirs.end());

            for (const auto& tag : tags)
                if (canonical (partsTagged (p, tag)) != canonical (partsTagged (t, tag)))
                    fields.push_back ({ "child:" + tag, fieldLabel ("child:" + tag) });

            if (placeOf (p, common) != placeOf (t, common))
                fields.push_back ({ "position", fieldLabel ("position") });

            return fields;
        }

        //  A sound in the performance's own media/ that the show's media/ does not have.
        void localSoundsOf (const juce::ValueTree& node, const juce::File& performance,
                            const juce::File& showFolder, std::vector<std::string>& out)
        {
            if (isCue (node))
            {
                const auto file = text (node, "file");

                if (! file.empty() && performance.getChildFile ("media").getChildFile (juce::String (file)).existsAsFile()
                      && ! showFolder.getChildFile ("media").getChildFile (juce::String (file)).existsAsFile()
                      && std::find (out.begin(), out.end(), file) == out.end())
                    out.push_back (file);
            }

            for (int i = 0; i < node.getNumChildren(); ++i)
                if (isItem (node.getChild (i)))
                    localSoundsOf (node.getChild (i), performance, showFolder, out);
        }

        /*  THE SHOW-WIDE SETTINGS: every container beside Lists, one item each,
            and the Lists' own attributes. A whole container is the unit,
            because its parts refer to each other - a bus to its outputs, a slot
            to its mount - and half of one is not a setting anybody chose. */
        std::string settingsLabel (const std::string& container)
        {
            static const std::map<std::string, std::string> words {
                { "Audio", "audio: outputs, buses and rack" }, { "Mounts", "devices on the network" },
                { "MidiPorts", "MIDI ports" }, { "Network", "network" }, { "Surfaces", "control surfaces" },
                { "Dcas", "DCAs" }, { "Lists", "list settings" }, { "Show", "show settings" },
            };

            const auto known = words.find (container);
            return known != words.end() ? known->second : container;
        }

        std::string attributesOf (const juce::ValueTree& node)
        {
            std::string said;

            for (int i = 0; i < node.getNumProperties(); ++i)
                said += node.getPropertyName (i).toString().toStdString() + "=" + node.getProperty (node.getPropertyName (i)).toString().toStdString() + ";";

            return said;
        }

        struct Opened
        {
            ShowDocument performance;
            ShowDocument showTemplate;
            juce::File performanceFolder;
            juce::File showFolder;
            std::string problem;
        };

        bool openBoth (const juce::File& performance, Opened& opened)
        {
            opened.performanceFolder = performance;
            opened.showFolder = showFolderOf (performance);

            if (opened.showFolder == juce::File())
            {
                opened.problem = performance.getFileName().toStdString() + " is not a performance: the folder around it is no show";
                return false;
            }

            if (! hasTemplate (opened.showFolder))
            {
                opened.problem = opened.showFolder.getFileName().toStdString() + " has no template cue list";
                return false;
            }

            if (const auto read = Bundle::open (performance, opened.performance); ! read.ok)
            {
                opened.problem = "the performance would not open: " + (read.problems.empty() ? std::string() : read.problems.front());
                return false;
            }

            if (const auto read = Bundle::open (opened.showFolder, opened.showTemplate); ! read.ok)
            {
                opened.problem = "the show's template would not open: " + (read.problems.empty() ? std::string() : read.problems.front());
                return false;
            }

            return true;
        }

        /*  Puts `copy` into `tParent` where `p` sits in the performance: after
            the item before it there, matched by id, and first when it is the
            first; at the end when that item is not in the template. */
        void placeLike (const juce::ValueTree& p, juce::ValueTree tParent, juce::ValueTree copy)
        {
            int index = -1;

            if (const auto before = previousItem (p); before.isValid())
            {
                for (int i = 0; i < tParent.getNumChildren(); ++i)
                    if (idOf (tParent.getChild (i)) == idOf (before))
                        index = i + 1;
            }
            else
            {
                //  First of its parent's items: after the parts, before every item.
                index = tParent.getNumChildren();

                for (int i = 0; i < tParent.getNumChildren(); ++i)
                    if (isItem (tParent.getChild (i))) { index = i; break; }
            }

            tParent.addChild (copy, index, nullptr);
        }

        //  The template's node for `p`'s parent: the Lists container, or the item with its id.
        juce::ValueTree parentInTemplate (const juce::ValueTree& p, const juce::ValueTree& tRoot)
        {
            const auto parent = p.getParent();

            if (tagOf (parent) == "Lists")
                return tRoot.getChildWithName ("Lists");

            return findIn (tRoot.getChildWithName ("Lists"), idOf (parent));
        }

        void copyIfMissing (const juce::File& from, const juce::File& to)
        {
            if (from.existsAsFile() && ! to.existsAsFile() && to.getParentDirectory().createDirectory())
                from.copyFileTo (to);
        }
    }

    //==========================================================================
    juce::File showFolderOf (const juce::File& document)
    {
        const auto around = document.getParentDirectory();

        if (around == document || ! around.isDirectory())
            return {};

        return (hasTemplate (around) || around.getChildFile ("media").isDirectory()) ? around : juce::File();
    }

    bool hasTemplate (const juce::File& showFolder)
    {
        return showFolder.isDirectory() && showFolder.getNumberOfChildFiles (juce::File::findFiles, "*.wfg") > 0;
    }

    TemplateComparison compare (const juce::File& performance)
    {
        TemplateComparison comparison;
        Opened opened;

        comparison.showFolder = showFolderOf (performance).getFullPathName().toStdString();
        comparison.hasTemplate = hasTemplate (showFolderOf (performance));

        if (! openBoth (performance, opened))
        {
            comparison.problem = opened.problem;
            return comparison;
        }

        const auto pRoot = opened.performance.root();
        const auto tRoot = opened.showTemplate.root();
        const auto pLists = pRoot.getChildWithName ("Lists");
        const auto tLists = tRoot.getChildWithName ("Lists");

        std::vector<juce::ValueTree> pItems, tItems;
        itemsUnder (pLists, pItems);
        itemsUnder (tLists, tItems);

        std::set<std::string> pIds, tIds, common;
        for (const auto& item : pItems) pIds.insert (idOf (item));
        for (const auto& item : tItems) tIds.insert (idOf (item));
        for (const auto& id : pIds) if (tIds.count (id) > 0) common.insert (id);

        //  Removed first, as the update applies them.
        for (const auto& t : tItems)
            if (pIds.count (idOf (t)) == 0 && (tagOf (t.getParent()) == "Lists" || pIds.count (idOf (t.getParent())) > 0))
                comparison.changes.push_back ({ TemplateChange::Kind::removed, idOf (t), labelOf (t), {}, {} });

        //  Settings.
        if (attributesOf (pRoot) != attributesOf (tRoot))
            comparison.changes.push_back ({ TemplateChange::Kind::settings, "Show", settingsLabel ("Show"), {}, {} });

        if (attributesOf (pLists) != attributesOf (tLists))
            comparison.changes.push_back ({ TemplateChange::Kind::settings, "Lists", settingsLabel ("Lists"), {}, {} });

        std::set<std::string> containers;
        for (int i = 0; i < pRoot.getNumChildren(); ++i) containers.insert (tagOf (pRoot.getChild (i)));
        for (int i = 0; i < tRoot.getNumChildren(); ++i) containers.insert (tagOf (tRoot.getChild (i)));
        containers.erase ("Lists");

        for (const auto& container : containers)
        {
            const auto p = pRoot.getChildWithName (juce::Identifier (container));
            const auto t = tRoot.getChildWithName (juce::Identifier (container));

            if (canonical ({ p }) != canonical ({ t }))
                comparison.changes.push_back ({ TemplateChange::Kind::settings, container, settingsLabel (container), {}, {} });
        }

        //  Added and changed, in the performance's order.
        for (const auto& p : pItems)
        {
            if (tIds.count (idOf (p)) == 0)
            {
                if (tagOf (p.getParent()) == "Lists" || tIds.count (idOf (p.getParent())) > 0)
                {
                    TemplateChange change { TemplateChange::Kind::added, idOf (p), labelOf (p), {}, {} };
                    localSoundsOf (p, performance, opened.showFolder, change.localSounds);
                    comparison.changes.push_back (std::move (change));
                }

                continue;
            }

            const auto t = findIn (tLists, idOf (p));

            if (auto fields = fieldsThatDiffer (p, t, common); ! fields.empty())
            {
                TemplateChange change { TemplateChange::Kind::changed, idOf (p), labelOf (p), std::move (fields), {} };

                if (isCue (p))
                {
                    const auto file = text (p, "file");
                    std::vector<std::string> sounds;
                    localSoundsOf (p, performance, opened.showFolder, sounds);

                    for (const auto& sound : sounds)
                        if (sound == file)
                            change.localSounds.push_back (sound);
                }

                comparison.changes.push_back (std::move (change));
            }
        }

        comparison.ok = true;
        return comparison;
    }

    TemplateUpdate update (const juce::File& performance, const std::vector<TemplatePick>& picks, bool copySounds)
    {
        TemplateUpdate result;
        const auto comparison = compare (performance);

        if (! comparison.ok)
        {
            result.said = comparison.problem;
            return result;
        }

        Opened opened;

        if (! openBoth (performance, opened))
        {
            result.said = opened.problem;
            return result;
        }

        if (app::OpenShow::heldElsewhere (opened.showFolder))
        {
            result.said = "the show's template is open in another window: close it there first";
            return result;
        }

        std::map<std::string, std::vector<std::string>> picked;
        for (const auto& pick : picks) picked[pick.id] = pick.fields;

        const auto wants = [&picked] (const std::string& id) { return picked.count (id) > 0; };
        const auto wantsField = [&picked] (const std::string& id, const std::string& field)
        {
            const auto& fields = picked.at (id);
            return fields.empty() || std::find (fields.begin(), fields.end(), field) != fields.end();
        };

        const auto pRoot = opened.performance.root();
        auto tRoot = opened.showTemplate.root();
        const auto pLists = pRoot.getChildWithName ("Lists");
        auto tLists = tRoot.getChildWithName ("Lists");

        std::vector<std::string> sounds;
        int applied = 0;
        bool mountsCopied = false;

        for (const auto& change : comparison.changes)
        {
            if (! wants (change.id) || change.kind != TemplateChange::Kind::removed)
                continue;

            if (auto t = findIn (tLists, change.id); t.isValid())
            {
                t.getParent().removeChild (t, nullptr);
                ++applied;
            }
        }

        for (const auto& change : comparison.changes)
        {
            if (! wants (change.id) || change.kind != TemplateChange::Kind::settings)
                continue;

            if (change.id == "Show" || change.id == "Lists")
            {
                auto t = change.id == "Show" ? tRoot : tLists;
                const auto p = change.id == "Show" ? pRoot : pLists;
                t.removeAllProperties (nullptr);

                for (int i = 0; i < p.getNumProperties(); ++i)
                    t.setProperty (p.getPropertyName (i), p.getProperty (p.getPropertyName (i)), nullptr);
            }
            else
            {
                const juce::Identifier container (change.id);
                const auto p = pRoot.getChildWithName (container);
                auto t = tRoot.getChildWithName (container);
                const auto index = t.isValid() ? tRoot.indexOf (t) : -1;

                if (t.isValid())
                    tRoot.removeChild (t, nullptr);

                if (p.isValid())
                    tRoot.addChild (p.createCopy(), index, nullptr);

                mountsCopied = mountsCopied || change.id == "Mounts";
            }

            ++applied;
        }

        for (const auto& change : comparison.changes)
        {
            if (! wants (change.id) || change.kind != TemplateChange::Kind::added)
                continue;

            const auto p = findIn (pLists, change.id);
            auto tParent = parentInTemplate (p, tRoot);

            if (! p.isValid() || ! tParent.isValid())
                continue;

            placeLike (p, tParent, p.createCopy());
            sounds.insert (sounds.end(), change.localSounds.begin(), change.localSounds.end());
            ++applied;
        }

        for (const auto& change : comparison.changes)
        {
            if (! wants (change.id) || change.kind != TemplateChange::Kind::changed)
                continue;

            const auto p = findIn (pLists, change.id);
            auto t = findIn (tLists, change.id);

            if (! p.isValid() || ! t.isValid())
                continue;

            for (const auto& field : change.fields)
            {
                if (! wantsField (change.id, field.name))
                    continue;

                if (field.name == "position")
                {
                    if (auto tParent = parentInTemplate (p, tRoot); tParent.isValid() && ! tParent.isAChildOf (t))
                    {
                        t.getParent().removeChild (t, nullptr);
                        placeLike (p, tParent, t);
                    }
                }
                else if (field.name.rfind ("child:", 0) == 0)
                {
                    const auto tag = field.name.substr (6);
                    int index = -1;

                    for (int i = t.getNumChildren(); --i >= 0;)
                        if (tagOf (t.getChild (i)) == tag)
                        {
                            index = i;
                            t.removeChild (i, nullptr);
                        }

                    if (index < 0)
                        for (int i = 0; i < t.getNumChildren() && index < 0; ++i)
                            if (isItem (t.getChild (i)))
                                index = i;

                    for (const auto& part : partsTagged (p, tag))
                        t.addChild (part.createCopy(), index < 0 ? -1 : index++, nullptr);
                }
                else
                {
                    const juce::Identifier name (field.name);

                    if (p.hasProperty (name))
                        t.setProperty (name, p.getProperty (name), nullptr);
                    else
                        t.removeProperty (name, nullptr);

                    if (field.name == "file")
                        sounds.insert (sounds.end(), change.localSounds.begin(), change.localSounds.end());
                }
            }

            ++applied;
        }

        if (applied == 0)
        {
            result.ok = true;
            result.said = "nothing was picked; the template is as it was";
            return result;
        }

        if (const auto problems = opened.showTemplate.validate(); ! problems.empty())
        {
            result.said = "the template was not changed - it would not be a valid show: " + problems.front();
            return result;
        }

        if (const auto saved = Bundle::save (opened.showFolder, opened.showTemplate); ! saved.ok)
        {
            result.said = "the template could not be written: " + (saved.problems.empty() ? std::string() : saved.problems.front());
            return result;
        }

        int copied = 0;

        if (copySounds)
            for (const auto& sound : sounds)
            {
                const auto to = opened.showFolder.getChildFile ("media").getChildFile (juce::String (sound));
                copyIfMissing (performance.getChildFile ("media").getChildFile (juce::String (sound)), to);
                copied += to.existsAsFile() ? 1 : 0;
            }

        //  A device brought in brings the description it is read through.
        if (mountsCopied)
            for (const auto& entry : juce::RangedDirectoryIterator (performance.getChildFile ("namespaces"), false, "*", juce::File::findFiles))
                copyIfMissing (entry.getFile(), opened.showFolder.getChildFile ("namespaces").getChildFile (entry.getFile().getFileName()));

        result.ok = true;
        result.said = std::to_string (applied) + (applied == 1 ? " change" : " changes") + " brought into "
                        + opened.showFolder.getFileName().toStdString() + "'s template"
                        + (copied > 0 ? ", with " + std::to_string (copied) + (copied == 1 ? " sound" : " sounds") : std::string())
                        + (! copySounds && ! sounds.empty() ? " - its sounds were left in the performance" : std::string());
        return result;
    }

    TemplateUpdate makeTemplate (const juce::File& performance)
    {
        TemplateUpdate result;
        const auto showFolder = showFolderOf (performance);

        if (showFolder == juce::File())
        {
            result.said = performance.getFileName().toStdString() + " is not a performance: the folder around it is no show";
            return result;
        }

        if (hasTemplate (showFolder))
        {
            result.said = showFolder.getFileName().toStdString() + " has a template already";
            return result;
        }

        ShowDocument document;

        if (const auto read = Bundle::open (performance, document); ! read.ok)
        {
            result.said = "the performance would not open: " + (read.problems.empty() ? std::string() : read.problems.front());
            return result;
        }

        if (const auto saved = Bundle::save (showFolder, document); ! saved.ok)
        {
            result.said = "the template could not be written: " + (saved.problems.empty() ? std::string() : saved.problems.front());
            return result;
        }

        for (const auto& entry : juce::RangedDirectoryIterator (performance.getChildFile ("namespaces"), false, "*", juce::File::findFiles))
            copyIfMissing (entry.getFile(), showFolder.getChildFile ("namespaces").getChildFile (entry.getFile().getFileName()));

        result.ok = true;
        result.said = performance.getFileName().toStdString() + " is now " + showFolder.getFileName().toStdString() + "'s template";
        return result;
    }

    std::vector<std::string> describe (const TemplateComparison& comparison)
    {
        std::vector<std::string> lines;

        for (const auto& change : comparison.changes)
        {
            std::string line;

            switch (change.kind)
            {
                case TemplateChange::Kind::added:    line = "+ " + change.label; break;
                case TemplateChange::Kind::removed:  line = "- " + change.label; break;
                case TemplateChange::Kind::settings: line = "= " + change.label; break;
                case TemplateChange::Kind::changed:
                {
                    line = "~ " + change.label + ":";

                    for (const auto& field : change.fields)
                        line += " " + field.label + (&field == &change.fields.back() ? "" : ",");

                    break;
                }
            }

            line += "  [" + change.id + "]";

            for (const auto& sound : change.localSounds)
                line += "  (only the performance has " + sound + ")";

            lines.push_back (line);
        }

        return lines;
    }
}
