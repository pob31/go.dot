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

#include <wfg/engine/document/Bundle.h>

#include <wfg/engine/document/EphemeralState.h>
#include <wfg/engine/document/Schema.h>

#include <string>

namespace wfg::doc
{
    namespace
    {
        constexpr const char* showFileName  = "show.xml";
        constexpr const char* stateFileName = "state.xml";
        constexpr const char* namespacesDir = "namespaces";
        constexpr const char* manifestSuffix = ".wfg";
        constexpr const char* manifestRoot  = "Bundle";

        std::string manifestText()
        {
            return "<" + std::string (manifestRoot) + " formatVersion=\""
                 + std::to_string (Schema::formatVersion()) + "\"/>\n";
        }

        /*  Raw bytes, never juce::File::replaceWithText.

            replaceWithText writes through a TextOutputStream, which on Windows
            turns every "\n" into "\r\n". Every file in a bundle is specified to
            use LF on all three platforms, so that a show written on Windows and
            one written on macOS are the same bytes - which is the only reason
            "open then save is byte-identical" is checkable at all. */
        bool writeBytes (const juce::File& file, const std::string& text, std::string& error)
        {
            juce::FileOutputStream stream { file };

            if (! stream.openedOk())
            {
                error = "cannot write " + file.getFullPathName().toStdString();
                return false;
            }

            stream.setPosition (0);
            stream.truncate();

            if (! stream.write (text.data(), text.size()))
            {
                error = "could not finish writing " + file.getFullPathName().toStdString();
                return false;
            }

            return true;
        }
    }

    //==============================================================================
    juce::File Bundle::manifestFile (const juce::File& folder)
    {
        return folder.getChildFile (folder.getFileName() + manifestSuffix);
    }

    juce::File Bundle::showFile (const juce::File& folder)
    {
        return folder.getChildFile (showFileName);
    }

    juce::File Bundle::stateFile (const juce::File& folder)
    {
        return folder.getChildFile (stateFileName);
    }

    juce::File Bundle::namespacesFolder (const juce::File& folder)
    {
        return folder.getChildFile (namespacesDir);
    }

    //==============================================================================
    ReadResult Bundle::open (const juce::File& folder, ShowDocument& document)
    {
        ReadResult result;

        if (! folder.isDirectory())
            return ReadResult::failed (folder.getFullPathName().toStdString()
                                       + " is not a folder; a show is a folder, not a file");

        //----------------------------------------------------------------------
        auto manifest = manifestFile (folder);

        if (! manifest.existsAsFile())
        {
            /*  The folder has been renamed and the manifest has not. Accepting
                it is the kind thing - the manifest carries one integer and
                nothing depends on its name - but saying so is the honest one,
                because the next `save` writes the folder-shaped name and the
                bundle would quietly acquire a second manifest. */
            juce::Array<juce::File> candidates;
            folder.findChildFiles (candidates, juce::File::findFiles, false,
                                   juce::String ("*") + manifestSuffix);

            if (candidates.isEmpty())
                return ReadResult::failed (folder.getFullPathName().toStdString()
                                           + " has no " + manifest.getFileName().toStdString()
                                           + "; it is not a Go.dot bundle");

            if (candidates.size() > 1)
                return ReadResult::failed (folder.getFullPathName().toStdString()
                                           + " has more than one manifest; it is not clear"
                                             " which show this folder is");

            manifest = candidates.getFirst();

            result.problems.push_back ("the manifest is " + manifest.getFileName().toStdString()
                                       + " but the folder is " + folder.getFileName().toStdString()
                                       + "; rename it to "
                                       + manifestFile (folder).getFileName().toStdString());
        }

        {
            juce::XmlDocument parser { manifest.loadFileAsString() };
            const auto xml = parser.getDocumentElement();

            if (xml == nullptr)
                return ReadResult::failed (manifest.getFileName().toStdString()
                                           + " is not valid XML: "
                                           + parser.getLastParseError().toStdString());

            if (xml->getTagName() != juce::String (manifestRoot))
                return ReadResult::failed (manifest.getFileName().toStdString()
                                           + "'s root element is <" + xml->getTagName().toStdString()
                                           + ">, expected <" + manifestRoot + ">");

            const auto version = xml->getIntAttribute ("formatVersion", 0);

            if (version > Schema::formatVersion())
                return ReadResult::failed (manifest.getFileName().toStdString()
                                           + " is format version " + std::to_string (version)
                                           + "; this build understands "
                                           + std::to_string (Schema::formatVersion()));
        }

        //----------------------------------------------------------------------
        const auto show = showFile (folder);

        if (! show.existsAsFile())
            return ReadResult::failed (folder.getFullPathName().toStdString() + " has no "
                                       + showFileName);

        const auto showResult = CanonicalXml::read (show.loadFileAsString().toStdString(), document);

        if (! showResult.ok)
        {
            /*  The document is untouched - CanonicalXml::read refuses rather
                than repairs - so whatever was open before this call is still
                open, and the caller has lost nothing by trying. */
            for (const auto& problem : showResult.problems)
                result.problems.push_back (std::string (showFileName) + ": " + problem);

            return result;
        }

        //----------------------------------------------------------------------
        /*  A bundle with no state.xml is normal and silent: a show that was
            never run, one whose state was deliberately not committed, one
            hand-written by somebody. Every ephemeral value stays at its
            default, which for a standby means "not set". */
        if (const auto state = stateFile (folder); state.existsAsFile())
        {
            const auto stateResult = EphemeralState::read (state.loadFileAsString().toStdString(),
                                                           document);

            for (const auto& problem : stateResult.problems)
                result.problems.push_back (problem);
        }

        result.ok = true;
        return result;
    }

    //==============================================================================
    ReadResult Bundle::save (const juce::File& folder, const ShowDocument& document)
    {
        ReadResult result;

        if (const auto created = folder.createDirectory(); created.failed())
            return ReadResult::failed ("cannot create " + folder.getFullPathName().toStdString()
                                       + ": " + created.getErrorMessage().toStdString());

        std::string error;

        /*  show.xml first. If the disk fills or the volume disappears, the file
            that matters is the one already written rather than the one still
            queued behind a manifest and a standby position. */
        if (! writeBytes (showFile (folder), CanonicalXml::write (document), error)
            || ! writeBytes (stateFile (folder), EphemeralState::write (document), error)
            || ! writeBytes (manifestFile (folder), manifestText(), error))
            return ReadResult::failed (error);

        result.ok = true;
        return result;
    }
}
