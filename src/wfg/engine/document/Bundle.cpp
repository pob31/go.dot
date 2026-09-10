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

#include <juce_cryptography/juce_cryptography.h>

#include <algorithm>
#include <string>
#include <vector>

#if defined (_WIN32)
 #include <process.h>        // _getpid
#else
 #include <unistd.h>         // getpid
#endif

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

        /*  One tick at 50 Hz. See the retry in `writeBytesAtomically` for what
            it costs and why it is this short. */
        constexpr int replaceRetryPauseMs = 20;

        std::string processIdText()
        {
           #if defined (_WIN32)
            return std::to_string (_getpid());
           #else
            return std::to_string (getpid());
           #endif
        }

        /*  A SAVE THAT CANNOT BE HALF-WRITTEN (PRD §4.3, namespace draft §14.10).

            What this replaced opened the real file, sought to zero, truncated
            it and only then wrote - and `truncate` FLUSHES before it shortens,
            so the empty file was committed to disk while the payload behind it
            was never explicitly flushed at all. A battery, a full disk or a
            closed lid in that window left a zero-length show.xml where the good
            one had been. Now the bytes go to a sibling, the sibling is made
            durable, and only then does it take the target's place.

            RAW BYTES STILL, never juce::File::replaceWithText. That writes
            through a TextOutputStream, which on Windows turns every "\n" into
            "\r\n", and every file in a bundle is specified LF on all three
            platforms, so that a show written on Windows and one written on
            macOS are the same bytes - which is the only reason "open then save
            is byte-identical" is checkable at all. The temp is written exactly
            as the target used to be.

            THE SAME DIRECTORY IS THE LOAD-BEARING PART, and JUCE is the reason.
            `replaceFileIn` (juce_File.cpp:323-336) has three branches. A target
            that exists reaches `replaceInternal`: `ReplaceFile` on Windows and
            `rename(2)` on POSIX, which is atomic within one filesystem. Across
            a volume boundary the rename fails and JUCE falls back to a copy and
            a delete - and on Linux that copy deletes the destination FIRST and
            streams the bytes afterwards, which is the very hazard this function
            exists to remove, brought back by the call meant to remove it. A
            sibling is on the target's own volume. The second branch is a target
            that does not exist yet, which `replaceFileIn` hands to
            `moveFileTo`: the first save into a folder with no show.xml is a
            move, and nothing here claims it is atomic - there was no old file
            for it to destroy. The third, a temp that IS the target, cannot
            happen with a name that differs from the target's.

            WHAT A FAILURE LEAVES is the old target, whole, and the temp beside
            it. The target is never opened for writing here; `replaceFileIn`
            returns before its own `deleteFile()` when the replace fails, so the
            temp survives it too. That is the right failure: the show that was
            on disk at 18:00 is still the show on disk, and nothing an operator
            trusted has become shorter. The temp is left rather than deleted
            because nothing reads it - `open` reads show.xml by name,
            `contentHash` lists its files by name, and the manifest search is
            for `*.wfg` - and because the next save from this process truncates
            it and uses it again.

            TWO EXCEPTIONS, NEITHER OF THEM THIS FUNCTION'S, both worth writing
            down rather than finding. On POSIX a rename that fails for ANY
            reason, not only a volume boundary, falls through to JUCE's same
            copy-and-delete, so "the old target, whole" rests on rename(2) not
            refusing a same-directory replace in a folder where the copy's own
            delete would then succeed - and it has no ordinary reason to, the
            two needing the same permission on the same directory. On Windows,
            `ReplaceFile` documents one failure (ERROR_UNABLE_TO_MOVE_REPLACEMENT)
            after which, when no backup name was given - and JUCE gives none -
            the replaced file no longer exists and the replacement keeps its own
            name. The old show is gone then and the new one is not lost but
            displaced: whole, flushed, under the temp's name. The retry below,
            finding no target, becomes a plain move that puts it where it
            belongs.

            WHAT THIS DOES NOT DO is make the RENAME durable on POSIX, which
            takes an fsync of the directory and JUCE offers none. After a power
            cut at the wrong instant the directory may still name the old file:
            the old show, whole, which is the failure this is built to accept.
            What it can no longer be is a file with the new name and none of
            the new bytes, because the bytes were flushed before the name moved. */
        bool writeBytesAtomically (const juce::File& target, const std::string& text,
                                   std::string& error)
        {
            const auto temp = Bundle::temporaryFor (target);

            /*  THE STREAM IS SCOPED, and the closing brace is load-bearing: it
                is what closes the temp before the replace below. JUCE opens
                files for writing with FILE_SHARE_READ and no FILE_SHARE_DELETE
                (juce_Files_windows.cpp:429-433), and `ReplaceFile` has to move
                the temp, so a handle still open on it here would make our own
                save fail on Windows - and pass on POSIX, where nobody would
                notice until a Windows user did. */
            {
                juce::FileOutputStream stream { temp };

                if (! stream.openedOk())
                {
                    error = "cannot write " + temp.getFullPathName().toStdString();
                    return false;
                }

                /*  FileOutputStream opens an existing file at its END, and a
                    temp can exist: a replace that failed earlier in this
                    session left one with this very name. Truncating the TEMP
                    is harmless - it is the file nobody trusts - and it is done
                    only when there is something to cut, because `truncate`
                    flushes first and a flush is a trip to the disk that a
                    fresh, empty temp does not need. */
                if (stream.getPosition() > 0)
                {
                    stream.setPosition (0);

                    if (stream.truncate().failed())
                    {
                        error = "cannot start again on " + temp.getFullPathName().toStdString();
                        return false;
                    }
                }

                if (! stream.write (text.data(), text.size()))
                {
                    error = "could not finish writing " + temp.getFullPathName().toStdString();
                    return false;
                }

                /*  Flushed AND made durable: `flush` ends in FlushFileBuffers on
                    Windows and fsync on POSIX, so the bytes are on the disk
                    before the name can point at them. */
                stream.flush();

                if (stream.getStatus().failed())
                {
                    error = "could not finish writing " + temp.getFullPathName().toStdString();
                    return false;
                }
            }

            /*  AND THE LENGTH, read back off the disk. `flush` discards the
                result of writing out its last buffer (juce_FileOutputStream.cpp,
                `flushBuffer` inside `flush`), and a short write - the tail of a
                show meeting the end of a disk - need set no status at all. What is
                on the disk is the one answer that cannot have been dropped on
                the way, and the temp, not the target, is the file it is asked
                of. */
            if (temp.getSize() != static_cast<juce::int64> (text.size()))
            {
                error = "could not finish writing " + temp.getFullPathName().toStdString();
                return false;
            }

            if (temp.replaceFileIn (target))
                return true;

            /*  ONE BLIND RETRY, because the API will not say what went wrong.
                `replaceFileIn` returns a bool and swallows GetLastError, so
                "retry on a sharing violation" is a distinction nothing here can
                make.

                THE FILE AT RISK IS THE TARGET, NOT THE TEMP, and the obvious
                guess is the wrong way round. The temp is ours and was closed
                above; `ReplaceFile` fails while ANOTHER process holds show.xml
                open without FILE_SHARE_DELETE - an editor somebody left it open
                in, a search indexer, a sync client uploading the previous save.
                Those hold on for milliseconds and let go, which is why one
                retry is worth having and a loop is not: a failure that survives
                a pause is not a transient one, and the honest thing to do with
                it is refuse.

                THE PAUSE IS ON THE TICK THREAD, which is the thread GO shares.
                It is one tick long per write, and `Bundle::save` makes three,
                so a save whose every replace needs its retry costs three ticks
                of pause on top of its writes - the worst case, and a rare one,
                since each retry answers a different file being held. It is paid
                only on this failure path - a save that replaces first time never
                sleeps - and what it can cost a GO is lateness, never a block: a
                save is a command applied in the queue's order like any other,
                so a GO already queued is applied before it, and a GO arriving
                during it is drained by the next tick, exactly as it would be
                behind any slow command - a tick this one has made late by as
                much as it overran, the pause included. No lock is taken and
                nothing is dropped. A save is not on the GO path at all; it is
                a person's gesture that happens to share the thread, and 5.5's
                autosave, the first write the engine decides on by itself, is
                where §14.14 measures the cost properly (M23). */
            juce::Thread::sleep (replaceRetryPauseMs);

            if (temp.replaceFileIn (target))
                return true;

            error = "could not replace " + target.getFullPathName().toStdString()
                  + " with " + temp.getFullPathName().toStdString()
                  + "; whatever was there is untouched, and the new bytes are beside it";
            return false;
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

    juce::File Bundle::temporaryFor (const juce::File& target)
    {
        /*  The process identifier is in the name so that two engines saving
            into one folder - two `wfg serve` on the same bundle, which nothing
            forbids - write two temps rather than into each other's. Which of
            them wins the replace is then the ordinary last-writer question,
            and neither can hand the other a file made of both. */
        return target.getSiblingFile (target.getFileName() + ".tmp-" + processIdText());
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
    std::string Bundle::contentHash (const juce::File& folder)
    {
        if (! folder.isDirectory())
            return {};

        /*  Sorted, so two machines listing the same folder hash it the same
            way. A directory listing is in whatever order the filesystem feels
            like, and a hash that depended on that would differ between the
            machine that recorded a log and the machine replaying it. */
        std::vector<juce::File> files;

        for (const auto& name : { showFileName, stateFileName })
            if (const auto file = folder.getChildFile (name); file.existsAsFile())
                files.push_back (file);

        if (const auto namespaces = namespacesFolder (folder); namespaces.isDirectory())
        {
            juce::Array<juce::File> found;
            namespaces.findChildFiles (found, juce::File::findFiles, false);

            for (const auto& file : found)
                files.push_back (file);
        }

        std::sort (files.begin(), files.end(),
                   [&folder] (const juce::File& a, const juce::File& b)
                   {
                       return a.getRelativePathFrom (folder).toStdString()
                                < b.getRelativePathFrom (folder).toStdString();
                   });

        juce::MemoryOutputStream combined;

        for (const auto& file : files)
        {
            juce::MemoryBlock bytes;

            if (! file.loadFileAsData (bytes))
                return {};

            /*  Path, length, bytes. The path is in so a rename changes the
                hash; the length is in so no two files can be concatenated into
                something that hashes like a different pair. Forward slashes on
                every platform, or the same bundle would hash differently on
                Windows. */
            auto relative = file.getRelativePathFrom (folder);
            relative = relative.replaceCharacter ('\\', '/');

            combined.writeString (relative);
            combined.writeInt64 (static_cast<juce::int64> (bytes.getSize()));
            combined.write (bytes.getData(), bytes.getSize());
        }

        return juce::SHA256 (combined.getData(), combined.getDataSize()).toHexString().toStdString();
    }

    std::vector<std::string> Bundle::logHeaderLines (const juce::File& folder)
    {
        const auto hash = contentHash (folder);

        return { "bundle " + folder.getFileName().toStdString()
                   + " sha256:" + (hash.empty() ? std::string ("unreadable") : hash) };
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
            queued behind a manifest and a standby position.

            EACH WRITE IS ATOMIC ON ITS OWN, AND THE THREE ARE NOT ONE. What a
            crash between them costs is small by construction. Inside or after
            the first, nothing: either the new show is committed or the old
            one is. Between the first and the second, a standby and a focus as
            the previous save left them, which §3.20 keeps in a separate file
            precisely because losing them is not losing work. Between the
            second and the third, nothing at all for a bundle that had a
            manifest, since `manifestText` is a pure function of the format
            version and the third write is byte for byte what was there - the
            one casualty is a save into a folder that never had one, which the
            next `open` refuses as not a bundle. The dangerous windows were
            always INSIDE writes one and two, never between them, and those are
            what the temp-and-replace closes. */
        if (! writeBytesAtomically (showFile (folder), CanonicalXml::write (document), error)
            || ! writeBytesAtomically (stateFile (folder), EphemeralState::write (document), error)
            || ! writeBytesAtomically (manifestFile (folder), manifestText(), error))
            return ReadResult::failed (error);

        result.ok = true;
        return result;
    }

    //==============================================================================
    void registerBundleCommands (CommandRegistry& registry,
                                 ShowDocument& document,
                                 DocumentSession& session)
    {
        registry.add ({ "document.save",
                        "Writes the show back to the bundle it was loaded from.",
                        {},
                        true,
                        [&document, &session] (CommandContext&,
                                               const std::vector<osc::Value>& args)
                        {
                            const auto written = Bundle::save (session.folder, document);

                            /*  A failed write is REJECTED and not merely
                                reported. The log's `A` means "this happened",
                                and a save that did not reach the disk did not
                                happen - a replay reproducing it as applied
                                would be reproducing a lie, and an operator
                                reading a green log would believe their show was
                                on disk when it was not.

                                `write-failed` and not `bad-address`, which is
                                what this said until Phase 5 and which is the
                                wrong word for a full disk: it sends an operator
                                to check a path that was never in question. The
                                new word is true of every path out of
                                `Bundle::save` - the folder could not be created,
                                or one of show.xml, state.xml and the manifest
                                could not be written in full beside itself or
                                could not take its own place - and all of those
                                are bytes that did not reach the disk.

                                What it does not carry is WHICH of them, though
                                `save` built the sentence and named the file:
                                the Outcome has room for a code and not for a
                                diagnosis, so the full-disk operator is told
                                that much and no more. */
                            if (! written.ok)
                                return Outcome::rejected (reason::writeFailed);

                            /*  THE DOT GOES OUT HERE, AND ONLY HERE. Stamped
                                after the write reached the disk and never
                                before, so a refused save leaves it lit - the
                                show on screen is still not the show in the
                                folder, and a light that went out on the attempt
                                would be telling somebody their work was safe
                                while it was not.

                                A save that got as far as show.xml and failed on
                                state.xml does not stamp, although the show half
                                did land: the command is refused as a whole, and
                                a dot that went out on a refused command would
                                be one more thing an operator had to learn to
                                distrust. The price is a lit dot over a show.xml
                                that is in fact current, which the next save
                                that lands puts out. */
                            session.savedRevision = document.showRevision();

                            return Outcome::ok (args);
                        } });
    }
}
