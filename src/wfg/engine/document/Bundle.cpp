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
#include <utility>
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
        constexpr const char* recoveryDir   = "recovery";
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

        /*  The last component of a path written on ANY platform, as a name this
            one can create.

            Both separators, whichever machine this is: a log recorded on the
            Windows box and replayed on the Mac mini carries `D:\shows\archive`,
            and `juce::File` on macOS would read that as one long relative name
            with a colon in it. Trailing separators are dropped first, so
            `archive/` is `archive` and not nothing; a path that is nothing but
            separators, or a bare drive, becomes `copy` rather than an empty
            name that would make the copy's parent the copy. */
        juce::String lastComponentOf (const std::string& path)
        {
            const auto trimmed = juce::String (path).trimCharactersAtEnd ("/\\");
            const auto cut = juce::jmax (trimmed.lastIndexOfChar ('/'),
                                         trimmed.lastIndexOfChar ('\\'));
            const auto name = juce::File::createLegalFileName (trimmed.substring (cut + 1));

            return name.isEmpty() ? juce::String ("copy") : name;
        }

        /*  THE SHOW IS REPLACED WHOLE OR NOT AT ALL, which `CanonicalXml::read`
            on its own does not promise.

            It refuses a document it cannot parse or build before touching
            anything - but its last pass, `validate()`, runs AFTER `adopt`, so a
            show that builds and then fails a check only the whole document can
            make comes back `ok == false` having already replaced the one that
            was open. For a load into an empty document nobody can tell. For
            `document.revert` and `document.recover`, which replace the show an
            operator is looking at, it would be a refused command that changed
            the show anyway - its history cleared, and a log line saying nothing
            happened.

            So those two read into a scratch document, and the finished tree is
            handed over only when every step of the read succeeded. It still
            comes through `adopt`, the one hatch, so the history, the lock and
            the revision counters behave exactly as they do for any load: the
            counters bump forward, once, on the live document. A move would have
            handed it the scratch document's small numbers instead, and every
            revision-keyed cache in the engine would have read a show that went
            backwards as a show that had not changed since it was built. */
        template <typename Reader>
        ReadResult readThenAdopt (ShowDocument& document, Reader&& readInto)
        {
            ShowDocument scratch;
            auto result = readInto (scratch);

            if (result.ok)
                document.adopt (scratch.root(), std::move (scratch.ids()));

            return result;
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

    juce::File Bundle::recoveryFolder (const juce::File& folder)
    {
        return folder.getChildFile (recoveryDir);
    }

    juce::File Bundle::recoveryShowFile (const juce::File& folder)
    {
        return recoveryFolder (folder).getChildFile (showFileName);
    }

    juce::File Bundle::recoveryStateFile (const juce::File& folder)
    {
        return recoveryFolder (folder).getChildFile (stateFileName);
    }

    bool Bundle::hasRecovery (const juce::File& folder)
    {
        /*  The SHOW is the question, and the state beside it is not asked
            about. A recovery folder holding only a state.xml describes a
            standby position for a show nobody wrote down, which is nothing to
            adopt; the reverse - a show with no state - is a perfectly good
            recovery whose standby comes back at its default, exactly as a
            bundle with no state.xml opens. */
        return recoveryShowFile (folder).existsAsFile();
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
    ReadResult Bundle::saveRecovery (const juce::File& folder, const ShowDocument& document)
    {
        /*  THE BUNDLE IS NOT CREATED, AND THAT IS THE WHOLE DIFFERENCE FROM
            `save` (§14.10). Nobody asked for this write, so nothing about it may
            invent a place to put it: a bundle somebody moved, renamed or
            unplugged mid-session is a `write-failed` here and a new folder full
            of orphaned files in `save`. The header argues why that asymmetry is
            the right way round. */
        if (! folder.isDirectory())
            return ReadResult::failed (folder.getFullPathName().toStdString()
                                       + " is no longer a folder; nothing was written");

        const auto destination = recoveryFolder (folder);

        /*  The subfolder, on the other hand, IS created - it is a child of a
            bundle that demonstrably exists, and creating it is what makes the
            first autosave of a session work at all. */
        if (const auto created = destination.createDirectory(); created.failed())
            return ReadResult::failed ("cannot create " + destination.getFullPathName().toStdString()
                                       + ": " + created.getErrorMessage().toStdString());

        std::string error;

        /*  The same writer, the same order and the same reasons as `save`: the
            show first, because it is the file that matters, and each write
            atomic on its own so that a crash inside one leaves the previous
            autosave whole rather than a truncated file that reads as a torn
            show. There is no manifest, so there are two writes rather than
            three. */
        if (! writeBytesAtomically (recoveryShowFile (folder), CanonicalXml::write (document), error)
            || ! writeBytesAtomically (recoveryStateFile (folder), EphemeralState::write (document),
                                       error))
            return ReadResult::failed (error);

        ReadResult result;
        result.ok = true;
        return result;
    }

    ReadResult Bundle::openRecovery (const juce::File& folder, ShowDocument& document)
    {
        /*  A TORN AUTOSAVE COSTS NOTHING, which is the last promise in a chain
            of them. The atomic write is what makes a half-written
            recovery/show.xml very nearly impossible; reading through a scratch
            document is what makes it harmless when a platform or a power cut
            manages it anyway - and makes a show that parses but fails
            validation just as harmless (`readThenAdopt` says why that needs
            saying). Whatever was open before this call is still open,
            unchanged, and the operator has lost nothing by asking. */
        return readThenAdopt (document, [&folder] (ShowDocument& scratch)
        {
            ReadResult result;

            const auto show = recoveryShowFile (folder);

            if (! show.existsAsFile())
                return ReadResult::failed (folder.getFullPathName().toStdString() + " has no "
                                           + recoveryDir + "/" + showFileName);

            const auto showResult = CanonicalXml::read (show.loadFileAsString().toStdString(),
                                                        scratch);

            if (! showResult.ok)
            {
                for (const auto& problem : showResult.problems)
                    result.problems.push_back (std::string (recoveryDir) + "/" + showFileName
                                               + ": " + problem);

                return result;
            }

            /*  The state beside it is forgiving, as state.xml always is: a
                standby that points at nothing costs a standby, and is reported
                rather than allowed to cost somebody their afternoon. */
            if (const auto state = recoveryStateFile (folder); state.existsAsFile())
            {
                const auto stateResult = EphemeralState::read (state.loadFileAsString().toStdString(),
                                                               scratch);

                for (const auto& problem : stateResult.problems)
                    result.problems.push_back (problem);
            }

            result.ok = true;
            return result;
        });
    }

    bool Bundle::discardRecovery (const juce::File& folder)
    {
        const auto doomed = recoveryFolder (folder);

        /*  A FOLDER THAT WAS NEVER THERE IS A FOLDER THAT IS GONE. The caller
            that cares about the difference - `document.discardRecovery`, which
            refuses an empty gesture rather than pretending it worked - asks
            `hasRecovery` first; every other caller only wants it gone, and a
            clean exit that reported a failure for a bundle nobody ever
            autosaved would be reporting the ordinary case. */
        if (! doomed.exists())
            return true;

        return doomed.deleteRecursively();
    }

    ReadResult Bundle::saveCopy (const juce::File& destination, const ShowDocument& document,
                                 const juce::File& source)
    {
        /*  THE FILES `save` REFUSES, copied rather than written - and copied
            FIRST. They describe somebody else's programs; Go.dot only reads
            them, and a save that rewrote them would claim an authorship it does
            not have. A copy claims nothing, which is why this is
            save-plus-a-copy and has a name that says so.

            FIRST, BECAUSE THE MANIFEST IS WHAT MAKES A FOLDER A BUNDLE, and
            `save` writes it last. Copied after the save, the descriptions
            arrived in a folder that was already an openable bundle, so there was
            a window in which a reader saw a show whose mounts pointed at files
            still being written - and a crash in that window left exactly that on
            disk: a bundle that opens, with mounts that describe nothing. Copied
            before, the manifest's arrival means everything else has, and a crash
            part-way leaves a folder with no manifest, which `open` refuses
            outright rather than half-loads. *This ordering is a correction (PR
            5.5, 2026-09-11):* the first build copied afterwards, and the
            black-box driver read a namespace description at nought bytes
            because of it.

            A SOURCE WITH NO `namespaces/` IS AN ORDINARY CASE and not a
            failure: a show with no mounts has no descriptions to carry, and a
            replay's `--out` never has one, because nothing Go.dot writes puts
            them there.

            A copy that fails is REPORTED AND DOES NOT STOP THE SAVE. The show is
            what somebody asked to have written, and a refusal that withheld a
            good show.xml because its companions could not follow would be a
            worse answer than a copy whose mounts have to be pointed at their
            descriptions again - so the save still runs, `ok` is the save's, and
            the problem is in the list, which is the same asymmetry `open` draws
            between what costs somebody their work and what does not. */
        std::vector<std::string> copyProblems;

        if (const auto namespaces = namespacesFolder (source); namespaces.isDirectory())
            if (! namespaces.copyDirectoryTo (namespacesFolder (destination)))
                copyProblems.push_back ("could not copy " + namespaces.getFullPathName().toStdString()
                                        + " into " + destination.getFullPathName().toStdString());

        auto result = save (destination, document);

        for (auto& problem : copyProblems)
            result.problems.push_back (std::move (problem));

        return result;
    }

    //==============================================================================
    void registerBundleCommands (CommandRegistry& registry,
                                 ShowDocument& document,
                                 DocumentSession& session,
                                 const juce::File& copiesFolder)
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

                            /*  THE DOT GOES OUT HERE - and, since PR 5.5, in
                                `document.revert`, which is the other way for
                                the document and the folder to come to hold the
                                same show, and nowhere else. Stamped after the
                                write reached the disk and never before, so a
                                refused save leaves it lit - the show on screen
                                is still not the show in the folder, and a light
                                that went out on the attempt would be telling
                                somebody their work was safe while it was not.

                                A save that got as far as show.xml and failed on
                                state.xml does not stamp, although the show half
                                did land: the command is refused as a whole, and
                                a dot that went out on a refused command would
                                be one more thing an operator had to learn to
                                distrust. The price is a lit dot over a show.xml
                                that is in fact current, which the next save
                                that lands puts out. */
                            session.savedRevision = document.showRevision();

                            /*  AND THE RECOVERY FOLDER GOES, because the work
                                has become the show (§14.10). What `recovery/`
                                holds after this line would be an older copy of
                                a document that is now on disk in full, and the
                                next `wfg serve` on this bundle would greet the
                                operator with an offer to restore work they had
                                already saved - which is the offer that teaches
                                somebody to dismiss the question without reading
                                it.

                                EXCEPT WHILE AN EARLIER SESSION'S OFFER IS
                                UNANSWERED, where the reason does not hold. The
                                folder then holds somebody else's afternoon -
                                the autosave has not written over it, for the
                                same reason (DocumentSession.h, `recoveryFound`)
                                - and this save has not made THAT work the show.
                                It stays until somebody recovers or discards it,
                                and the next start offers it again, which costs
                                nothing: whatever it would replace is on disk as
                                the show and `document.revert` brings it back.

                                Its failure is not the save's. The bytes landed,
                                the dot is out, and a folder that would not
                                delete is a tidiness problem; refusing the save
                                over it would report a lie about the show. */
                            if (! session.recoveryFound)
                            {
                                Bundle::discardRecovery (session.folder);
                                session.autosavedRevision = 0;
                            }

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  `document.autosave` - THE ONE COMMAND IN THIS ENGINE NOBODY ASKS FOR
            (§14.8, §14.10).

            ORIGIN `engine`, DECIDED BY A HOOK, APPLIED BY THIS HANDLER, and the
            split is Phase 3's rule rather than a new one: the hook decides, the
            handler applies. The arithmetic over the session - now, rather than
            in a second's time - is a decision taken by something watching the
            clock, and a replay runs no hooks; a replay that re-derived it would
            be a second implementation of one judgement, drifting from the first
            the moment either changed. So the record carries the tick and the
            replay re-injects it, which is what makes a replayed session write
            the same files at the same ticks as the session it replays.

            REPLAY-IDEMPOTENT, which is what lets it be registered at all: it
            reads the document and writes two files, so applying it twice from
            the same document produces the same bytes twice.

            NO ARGUMENTS. What it wrote is not in the record because a record of
            the bytes would be a record of something nobody decided - the same
            argument `audio.editBuilt` makes, whose comment is the sentence to
            keep: the graph exists, and that is an event, not a variable being
            set. A save that happened is an event. The bytes it wrote are not. */
        registry.add ({ "document.autosave",
                        "Writes the show to the bundle's recovery folder. The engine decides this.",
                        {},
                        true,
                        [&document, &session] (CommandContext& context,
                                               const std::vector<osc::Value>& args)
                        {
                            /*  STAMPED FIRST, AND ON BOTH PATHS, because what
                                this clock measures is the ATTEMPT. A refused
                                autosave leaves `autosavedRevision` where it
                                was, so the "is there anything to write" half of
                                `autosaveDue` stays true; without this stamp the
                                quiet term would stay true with it and a bundle
                                on a stick somebody pulled out would write a
                                refused record fifty times a second, for as long
                                as the show stayed open. With it, the same
                                bundle is one refusal every thirty seconds. */
                            session.lastAutosaveTick = context.tick;

                            const auto written = Bundle::saveRecovery (session.folder, document);

                            /*  Refused and not merely reported, for the reason
                                `document.save` is: the log's `A` means "this
                                happened", and an autosave that did not reach
                                the disk did not happen. It matters more here
                                than there, because nobody is watching - the
                                only trace an unattended writer leaves is its
                                record, and a record that says a crash-safe copy
                                exists when it does not is worse than no copy. */
                            if (! written.ok)
                                return Outcome::rejected (reason::writeFailed);

                            /*  WHAT `recovery/` NOW HOLDS. It is not the dot:
                                the document is still dirty, and truthfully so,
                                because what is on disk as the SHOW is still
                                what it was. This says only that the same bytes
                                need not be written again until something moves,
                                which is what keeps a dirty, idle show from
                                autosaving on every tick after its two seconds
                                of quiet. */
                            session.autosavedRevision = document.showRevision();

                            /*  AND THE LATCH IS DELIBERATELY NOT TOUCHED. This
                                session's own autosave is this session's own
                                work, already in the document in front of the
                                operator; `/godot/document/recovery` reports
                                somebody ELSE's unfinished afternoon. Lit here,
                                every unsaved show would offer to restore itself
                                two seconds after the first edit. */
                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  `document.recover` - THAT THE ABANDONED AFTERNOON WAS WORTH KEEPING,
            which is a decision the engine will not take on anybody's behalf.

            The engine is headless and has nobody to ask, so on open it says the
            folder is there and does nothing else (§14.10). Adopting it silently
            would be wrong three times over: the show on screen would differ
            from the file the operator opened with no gesture in between; it
            would decide for them that the afternoon was worth keeping, which is
            the one decision autosave exists to leave open; and `adopt` replaces
            the root wholesale, lock included, so a silent adopt could unlock a
            locked show because a file on disk said so - during a performance. */
        registry.add ({ "document.recover",
                        "Adopts the bundle's recovery folder, replacing the open show.",
                        {},
                        true,
                        [&document, &session] (CommandContext&,
                                               const std::vector<osc::Value>& args)
                        {
                            /*  ASKED IN THE HANDLER, like undo's, because this
                                knocks at no door: `adopt` is a hatch that
                                replaces the show and the lock with it, so the
                                four predicates never see it (§14.11). Asked
                                FIRST, before a byte is read, so that a locked
                                show answers `locked` rather than reading a file
                                it will not use. */
                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            /*  THE DISK IS ASKED AND NOT THE LATCH. A client
                                that has already recovered once this session and
                                asks again is answered by what is there, not by
                                what this session remembers - and the two differ
                                exactly when somebody else has been writing into
                                the folder, which is a case worth being honest
                                in rather than clever about. */
                            const auto recovered = Bundle::openRecovery (session.folder, document);

                            /*  ONE WORD FOR BOTH FAILURES, and it is the word
                                §14.7's table gives this command. A torn
                                recovery/show.xml is nothing anybody can adopt,
                                which is what `no-recovery` says; the vocabulary
                                for this command has two codes in it and
                                inventing a third for a file the atomic write
                                makes very nearly impossible would be adding a
                                clause to a contract nothing would ever write.
                                The document is untouched either way. */
                            if (! recovered.ok)
                                return Outcome::rejected (reason::noRecovery);

                            /*  THE DOT STAYS LIT, DELIBERATELY. The recovered
                                work is not on disk as the show, and the dot is
                                telling the truth: `savedRevision` is not
                                re-stamped, which is `document.revert`'s rule
                                read the other way round.

                                The history went with the show it was about -
                                `adopt` clears it before the swap, because every
                                action on the stack holds a handle into a tree
                                that is about to be replaced. */
                            session.autosavedRevision = document.showRevision();
                            session.recoveryFound = false;

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        registry.add ({ "document.discardRecovery",
                        "Deletes the bundle's recovery folder.",
                        {},
                        true,
                        [&session] (CommandContext&, const std::vector<osc::Value>& args)
                        {
                            /*  NOT REFUSED BY THE LOCK. It writes bytes - it
                                deletes them, rather - and touches the document
                                not at all, so it belongs with the save and the
                                copy in §14.7's "keeps working" row.

                                An empty gesture is refused rather than reported
                                as done, because a client that cannot tell "I
                                deleted it" from "there was nothing there" is a
                                client that shows the operator a folder it has
                                just failed to remove.

                                THE FOLDER IS ASKED, NOT THE SHOW FILE IN IT,
                                which is where this differs from `recover`. A
                                `recovery/` holding only the temp an interrupted
                                write left behind has nothing to adopt and is
                                still something to delete - §14.7 says
                                `no-recovery` of a bundle with no folder, and
                                this has one. */
                            if (! Bundle::recoveryFolder (session.folder).isDirectory())
                                return Outcome::rejected (reason::noRecovery);

                            if (! Bundle::discardRecovery (session.folder))
                                return Outcome::rejected (reason::writeFailed);

                            session.autosavedRevision = 0;
                            session.recoveryFound = false;

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  `document.revert` - THAT THE BUNDLE ON DISK WINS.

            The undo of everything, for the session that decides an afternoon
            was wrong. It clears the history, so nothing before it is undoable
            either, which is the one property that makes it different in kind
            from an undo rather than a large one. */
        registry.add ({ "document.revert",
                        "Loads the bundle from disk again, discarding every change since the last save.",
                        {},
                        true,
                        [&document, &session] (CommandContext&,
                                               const std::vector<osc::Value>& args)
                        {
                            if (document.isLocked())
                                return Outcome::rejected (reason::locked);

                            /*  Through a scratch document, so that a refused
                                revert is a revert that changed nothing - not
                                one that replaced the show and then reported a
                                validation failure (`readThenAdopt`). */
                            const auto reloaded = readThenAdopt (
                                document,
                                [&session] (ShowDocument& scratch)
                                {
                                    return Bundle::open (session.folder, scratch);
                                });

                            /*  `bad-address` and not `write-failed`: nothing was
                                being written, and what failed is that the
                                folder this session names is no longer a
                                readable bundle - which IS a fact about a path,
                                and is the one refusal in this file that
                                genuinely sends somebody to look at one. */
                            if (! reloaded.ok)
                                return Outcome::rejected (reason::badAddress);

                            /*  AND IT DOES NOT LEAVE THE DOT LIT, which takes a
                                re-stamp rather than nothing at all. `adopt`
                                ends in "a load is the largest change there is"
                                and bumps the show counter, so a revert that did
                                not re-stamp would report unsaved changes at the
                                instant the document matched the disk.
                                `document.recover` re-stamps nothing, for the
                                same reason read the other way: the show on disk
                                is NOT what the document now holds.

                                AND `recovery/` GOES ON `document.save`'s TERMS,
                                because after this line the document and the
                                folder hold the same show, which is the reason
                                the save gives. What the folder holds is then a
                                copy of the very work the operator has just
                                thrown away, and a crash before the next edit
                                would have the next start offer it back to them.
                                Not while an earlier session's offer stands,
                                though: a revert says the bundle wins, and says
                                nothing about somebody else's afternoon, so the
                                latch and its folder are left for an answer
                                (DocumentSession.h, `recoveryFound`). */
                            session.savedRevision = document.showRevision();

                            if (! session.recoveryFound)
                            {
                                Bundle::discardRecovery (session.folder);
                                session.autosavedRevision = 0;
                            }

                            return Outcome::ok (args);
                        } });

        //----------------------------------------------------------------------
        /*  `document.saveAs <path>` - THAT THESE BYTES ARE ALSO SOMEWHERE ELSE.

            IT DOES NOT RE-POINT THE SESSION, and that is the half most likely
            to be argued about (plan decision 7, here to be overruled early
            rather than late). A "save a copy for the archive" gesture that
            silently makes the archive the live document is a trap with a delay
            fuse: the operator's next Ctrl-S goes somewhere they did not name,
            and they find out at the next load. A client that wants to work in
            the copy opens it, and opening is a restart. */
        registry.add ({ "document.saveAs",
                        "Writes the show into another folder, as a copy. The session keeps its own.",
                        { { "path", 's', false } },
                        true,
                        [&document, &session, copiesFolder] (CommandContext&,
                                                             const std::vector<osc::Value>& args)
                        {
                            /*  Indexed without a guard, as every handler with a
                                required parameter is: the registry has already
                                refused a call that arrived without one, with
                                `arity`, before this ran. */
                            const auto path = args[0].getString();

                            if (path.empty())
                                return Outcome::rejected (reason::badAddress);

                            /*  RESOLVED AGAINST THE WORKING DIRECTORY, exactly
                                as every path this binary is handed on a command
                                line is. `getChildFile` returns an absolute path
                                unchanged, so a client that knows where it wants
                                the copy says so in full and a script run beside
                                its show may say `archive/tuesday`. A bare
                                juce::File on a relative string is an assertion
                                in a debug build and an empty path in a release
                                one, which is the failure this line exists to
                                not have.

                                Unless there is a copies folder, which only a
                                replay supplies; the declaration says why. */
                            const auto destination =
                                copiesFolder.getFullPathName().isEmpty()
                                  ? juce::File::getCurrentWorkingDirectory().getChildFile (path)
                                  : copiesFolder.getChildFile ("saveAs")
                                                .getChildFile (lastComponentOf (path));

                            const auto written = Bundle::saveCopy (destination, document,
                                                                   session.folder);

                            if (! written.ok)
                                return Outcome::rejected (reason::writeFailed);

                            /*  NO STAMP OF ANY KIND. The session still points at
                                the folder it opened, whose show.xml is still
                                behind this document, so the dot stays exactly
                                as it was - which is the truth, and is what
                                makes this a copy rather than a save. */
                            return Outcome::ok (args);
                        } });
    }
}
