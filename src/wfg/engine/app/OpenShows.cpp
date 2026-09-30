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

#include <wfg/engine/app/OpenShows.h>

#include <wfg/engine/app/Raise.h>

#include <cstdint>
#include <mutex>
#include <set>
#include <string>

namespace wfg::app
{
    namespace
    {
        /*  FNV-1a over the folded path: stable from one run and one build to
            the next, which std::hash does not promise, and short enough for a
            mutex's name. */
        juce::String keyOf (const juce::File& show)
        {
            auto path = show.getFullPathName();

            while (path.length() > 1 && (path.endsWithChar ('/') || path.endsWithChar ('\\')))
                path = path.dropLastCharacters (1);

           #if JUCE_WINDOWS || JUCE_MAC
            path = path.toLowerCase();
           #endif

            std::uint64_t hash = 14695981039346656037ull;

            for (const auto byte : path.toStdString())
            {
                hash ^= static_cast<std::uint8_t> (byte);
                hash *= 1099511628211ull;
            }

            return juce::String::toHexString (static_cast<juce::int64> (hash)).paddedLeft ('0', 16);
        }

        juce::String lockNameOf (const juce::String& key)
        {
            return "GoDot-show-" + key;
        }

        /*  The shows this process holds. Asked before the system is: a second
            claim by this process must fail, which a Windows mutex does and an
            fcntl lock does not, and a probe of one of them must never be made
            (heldElsewhere says why). */
        std::mutex heldHereMutex;
        std::set<std::string> heldHere;

        bool isHeldHere (const juce::String& key)
        {
            const std::lock_guard<std::mutex> guard (heldHereMutex);
            return heldHere.count (key.toStdString()) > 0;
        }
    }

    juce::File OpenShow::defaultEntries()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                 .getChildFile ("Go.dot").getChildFile ("open");
    }

    juce::File OpenShow::entryFor (const juce::File& show, const juce::File& entries)
    {
        return entries.getChildFile (keyOf (show) + ".pid");
    }

    OpenShow::OpenShow (const juce::String& keyToHold, const juce::File& entryToWrite)
        : key (keyToHold), lock (lockNameOf (keyToHold)), entry (entryToWrite)
    {
    }

    std::unique_ptr<OpenShow> OpenShow::claim (const juce::File& show, const juce::File& entries)
    {
        const auto key = keyOf (show);

        //  Held across the claim, so two in this process are taken in turn.
        const std::lock_guard<std::mutex> guard (heldHereMutex);

        if (heldHere.count (key.toStdString()) > 0)
            return nullptr;

        std::unique_ptr<OpenShow> open { new OpenShow (key, entryFor (show, entries)) };

        //  Refused, it is let go of unclaimed, which touches nothing (the destructor).
        if (! open->lock.enter (0))
            return nullptr;

        open->claimed = true;
        heldHere.insert (key.toStdString());

        /*  After the lock, never before: a file written by a process that
            then lost the race would name the wrong window. A file that cannot
            be written costs only the raise, so it is not fatal. */
        entries.createDirectory();
        open->entry.replaceWithText (juce::String (currentProcessId()));

        return open;
    }

    OpenShow::~OpenShow()
    {
        /*  Never having held the show, it has nothing to give back - and
            `claim` may be letting it go with heldHereMutex still in hand. */
        if (! claimed)
            return;

        entry.deleteFile();
        lock.exit();

        const std::lock_guard<std::mutex> guard (heldHereMutex);
        heldHere.erase (key.toStdString());
    }

    bool OpenShow::heldElsewhere (const juce::File& show)
    {
        const auto key = keyOf (show);

        if (isHeldHere (key))
            return false;

        juce::InterProcessLock probe (lockNameOf (key));

        if (probe.enter (0))
        {
            probe.exit();
            return false;
        }

        return true;
    }

    bool OpenShow::raiseHolder (const juce::File& show, const juce::File& entries)
    {
        const auto named = entryFor (show, entries).loadFileAsString().trim();

        if (named.isEmpty() || ! named.containsOnly ("0123456789"))
            return false;

        return raiseProcessWindows (static_cast<long> (named.getLargeIntValue()));
    }
}
