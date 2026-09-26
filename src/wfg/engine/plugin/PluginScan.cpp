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

#include <wfg/engine/plugin/PluginScan.h>

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
#else
 #include <csignal>
 #include <sys/types.h>
 #include <unistd.h>
#endif

#if JUCE_MAC
/*  As PluginHostChild.cpp and Console.cpp: the loop below is [NSApp run]. */
namespace juce { void initialiseNSApplication(); }
#endif

namespace wfg::plugin
{
    namespace te = tracktion::engine;

    namespace
    {
        /*  THE SAME STORAGE AudioHost STANDS ITS ENGINE ON - the same app name,
            the same folder, so the list a scan writes is the list serve reads
            at the next open. Restated here rather than shared, because
            AudioHost's is private to that file and this one needs no device,
            no edit and no graph: an engine for its plugin manager only. */
        struct ScanStorage final : te::PropertyStorage
        {
            explicit ScanStorage (juce::File root)
                : te::PropertyStorage ("Go.dot"), folder (std::move (root))
            {
                folder.createDirectory();
            }

            juce::File getAppCacheFolder() override  { return folder; }
            juce::File getAppPrefsFolder() override  { return folder; }
            juce::String getApplicationVersion() override { return WFG_VERSION; }

            juce::File folder;
        };

        struct ScanBehaviour final : te::EngineBehaviour
        {
            bool autoInitialiseDeviceManager() override { return false; }

            /** The whole point of this file. */
            bool canScanPluginsOutOfProcess() override { return true; }
        };

        std::unique_ptr<te::Engine> engineOn (const std::string& storageFolder)
        {
            return std::make_unique<te::Engine> (std::make_unique<ScanStorage> (juce::File (juce::String (storageFolder))),
                                                 std::make_unique<te::UIBehaviour>(),
                                                 std::make_unique<ScanBehaviour>());
        }

        KnownPlugin knownFrom (const juce::PluginDescription& description)
        {
            KnownPlugin out;
            out.name = description.name.toStdString();
            out.identifier = description.createIdentifierString().toStdString();
            out.format = formatWordOf (description.pluginFormatName.toStdString());
            out.manufacturer = description.manufacturerName.toStdString();
            out.path = description.fileOrIdentifier.toStdString();

            if (const auto xml = description.createXml())
                out.description = xml->toString().toStdString();

            return out;
        }

        std::vector<KnownPlugin> knownFrom (const juce::KnownPluginList& list)
        {
            std::vector<KnownPlugin> out;

            for (const auto& description : list.getTypes())
                out.push_back (knownFrom (description));

            std::sort (out.begin(), out.end(), [] (const KnownPlugin& a, const KnownPlugin& b)
            {
                return a.name < b.name || (a.name == b.name && a.identifier < b.identifier);
            });

            return out;
        }

        /*  Tracktion's key for the list inside its Settings.xml, on a 64-bit
            build - the only kind this project makes. Read once, for the
            import below, and never written. */
        constexpr const char* tracktionListKey = "knownPluginList64";

        /*  THE LIST AS THE MACHINE KNOWS IT: known.xml, or - on a machine that
            scanned before that file existed - what Tracktion's Settings.xml
            holds, which is where the scans of Phase 9a left it. A plain parse:
            a properties file stores an XML value as the child of its VALUE
            element. False when neither file says anything. */
        bool readList (const juce::File& storage, juce::KnownPluginList& list)
        {
            if (const auto own = juce::parseXML (juce::File (juce::String (knownListPath (storage.getFullPathName().toStdString()))));
                own != nullptr)
            {
                list.recreateFromXml (*own);
                return true;
            }

            if (const auto settings = juce::parseXML (storage.getChildFile ("Settings.xml")); settings != nullptr)
                if (const auto* value = settings->getChildByAttribute ("name", tracktionListKey))
                    if (const auto* held = value->getFirstChildElement())
                    {
                        list.recreateFromXml (*held);
                        return true;
                    }

            return false;
        }

        /*  Written WHOLE, by replacement: JUCE writes a temporary beside the
            file and moves it over, so a reader never meets half a list. */
        bool writeList (const juce::File& storage, const juce::KnownPluginList& list)
        {
            const juce::File file { juce::String (knownListPath (storage.getFullPathName().toStdString())) };
            file.getParentDirectory().createDirectory();

            if (const auto xml = list.createXml())
                return xml->writeTo (file);

            return false;
        }

        bool formatMatches (const juce::AudioPluginFormat& format, const std::string& word)
        {
            if (word.empty())
                return true;

            const auto name = format.getName().toLowerCase().toStdString();

            if (word == "vst3") return name == "vst3";
            if (word == "au")   return name.rfind ("audiounit", 0) == 0;
            if (word == "lv2")  return name == "lv2";

            return false;
        }

        /*  WHAT THE SCAN CHILD IS CALLED WITH. Tracktion's coordinator launches
            this executable with JUCE's `--<uid>:<pipe>` option, and its uid,
            `PluginScan`, sits in a header the engine keeps to itself
            (tracktion_PluginScanHelpers.h, included by one .cpp and exported
            by none). Restated here rather than reached for; a drift fails
            loudly - the child would read its pipe option as a verb and exit
            non-zero, which the coordinator counts as a crash - never
            silently. `startChildProcessPluginScan` checks the same prefix. */
        constexpr const char* scanChildOption = "--PluginScan:";

        /*  WHERE THE CHILD LEAVES ITS PID, told through the environment
            because Tracktion launches it with the pipe option and nothing
            else, and a child inherits the environment it was launched from.
            Needed because JUCE's coordinator never terminates a worker: its
            kill is a MESSAGE on the pipe, and a child hung inside a plugin's
            code never reads it - the first scan on this machine left one
            behind at a gigabyte, owned by nobody, after the parent had gone.
            The parent terminates by pid what the message could not reach. */
        constexpr const char* pidFileVariable = "WFG_SCAN_PID_FILE";

        std::int64_t currentProcessId()
        {
           #if JUCE_WINDOWS
            return static_cast<std::int64_t> (::GetCurrentProcessId());
           #else
            return static_cast<std::int64_t> (::getpid());
           #endif
        }

        bool processIsAlive (std::int64_t pid)
        {
            if (pid <= 0)
                return false;

           #if JUCE_WINDOWS
            auto* handle = ::OpenProcess (SYNCHRONIZE, FALSE, static_cast<DWORD> (pid));

            if (handle == nullptr)
                return false;

            const auto alive = ::WaitForSingleObject (handle, 0) == WAIT_TIMEOUT;
            ::CloseHandle (handle);
            return alive;
           #else
            return ::kill (static_cast<pid_t> (pid), 0) == 0;
           #endif
        }

        void terminateProcess (std::int64_t pid)
        {
            if (pid <= 0 || pid == currentProcessId())
                return;

           #if JUCE_WINDOWS
            if (auto* handle = ::OpenProcess (PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD> (pid)))
            {
                ::TerminateProcess (handle, 1);
                ::WaitForSingleObject (handle, 2000);
                ::CloseHandle (handle);
            }
           #else
            ::kill (static_cast<pid_t> (pid), SIGKILL);
           #endif
        }

        void setEnvironment (const char* name, const juce::String& value)
        {
           #if JUCE_WINDOWS
            ::_putenv_s (name, value.toRawUTF8());
           #else
            ::setenv (name, value.toRawUTF8(), 1);
           #endif
        }

        /*  The pid the last child wrote, or nought. Read by the parent when
            it has given a child up, and once more when the scan is over. */
        std::int64_t childPidFrom (const juce::File& pidFile)
        {
            return pidFile.existsAsFile() ? pidFile.loadFileAsString().trim().getLargeIntValue() : 0;
        }

        /*  Gives the child JUCE's message first - a healthy one exits on it -
            and terminates by pid what is still there after a moment. */
        void putDownTheChild (juce::KnownPluginList& list, const juce::File& pidFile)
        {
            const auto pid = childPidFrom (pidFile);
            list.scanFinished();

            for (int waited = 0; waited < 20 && processIsAlive (pid); ++waited)
                juce::Thread::sleep (25);

            if (processIsAlive (pid))
                terminateProcess (pid);

            pidFile.deleteFile();
        }

        struct StopTheLoop final : juce::Timer
        {
            void timerCallback() override
            {
                stopTimer();
                juce::MessageManager::getInstance()->stopDispatchLoop();
            }
        };

        /*  The scan itself, and IT OWNS THE FILE LOOP rather than handing it
            to juce::PluginDirectoryScanner, because the loop is where the
            deadline lives. Each file goes to Tracktion's custom scanner - which
            is what talks to the child - with its start time published for the
            watchdog below; a file the watchdog claims is one that did not
            answer in time, and it is blacklisted, the child put down through
            `scanFinished`, and the next file scanned with a fresh one. The
            dead man's pedal is JUCE's idea kept by hand: the file being scanned
            is written before and cleared after, so one that took THIS process
            down is blacklisted by the next scan's first act. Posts the loop's
            stop when done, behind the known list's last change message. */
        struct ScanThread final : juce::Thread
        {
            ScanThread (te::PluginManager& managerToUse, std::string formatWordToUse,
                        std::string extraFolderToUse, juce::File storageToUse)
                : juce::Thread ("wfg plugin scan"),
                  manager (managerToUse),
                  formatWord (std::move (formatWordToUse)),
                  extraFolder (std::move (extraFolderToUse)),
                  pedal (storageToUse.getChildFile ("plugin-scan.pedal")),
                  pidFile (storageToUse.getChildFile ("plugin-scan.child"))
            {
                setEnvironment (pidFileVariable, pidFile.getFullPathName());
            }

            void run() override
            {
                auto& list = manager.knownPluginList;

                for (const auto& crashed : juce::StringArray::fromLines (pedal.loadFileAsString()))
                    if (crashed.trim().isNotEmpty())
                    {
                        list.addToBlacklist (crashed.trim());
                        skipped.push_back (crashed.trim().toStdString());
                    }

                pedal.deleteFile();

                for (int i = 0; i < manager.pluginFormatManager.getNumFormats() && ! threadShouldExit(); ++i)
                {
                    auto* format = manager.pluginFormatManager.getFormat (i);

                    if (format == nullptr || ! formatMatches (*format, formatWord))
                        continue;

                    /*  Tracktion's built-in format has nothing to scan, and
                        asking it for default locations answers nothing;
                        skipped by name. */
                    if (format->getName() == te::PluginManager::builtInPluginFormatName)
                        continue;

                    auto where = format->getDefaultLocationsToSearch();

                    if (! extraFolder.empty())
                        where.addIfNotAlreadyThere (juce::File (juce::String (extraFolder)));

                    if (where.getNumPaths() == 0)
                        continue;

                    for (const auto& file : format->searchPathsForPlugins (where, true, false))
                    {
                        if (threadShouldExit())
                            break;

                        if (list.getBlacklistedFiles().contains (file) || list.isListingUpToDate (file, *format))
                            continue;

                        pedal.replaceWithText (file, false, false, "\n");
                        fileStartedMs.store (std::max<std::uint32_t> (1u, juce::Time::getMillisecondCounter()),
                                             std::memory_order_release);

                        juce::OwnedArray<juce::PluginDescription> found;
                        list.scanAndAddFile (file, true, found, *format);

                        /*  Nought back means the watchdog claimed the file
                            while the child was on it: the scan under it was
                            aborted. One that answered at the very edge is in
                            the list and is kept; one that did not is
                            blacklisted. Either way the abort has to be undone
                            and the child put down, which is what Tracktion's
                            scanFinished does - the next file gets a new one. */
                        const auto claimed = fileStartedMs.exchange (0, std::memory_order_acq_rel) == 0;
                        pedal.deleteFile();

                        if (claimed)
                        {
                            if (! list.isListingUpToDate (file, *format))
                            {
                                list.addToBlacklist (file);
                                skipped.push_back (file.toStdString());
                            }

                            putDownTheChild (list, pidFile);
                        }
                    }
                }

                putDownTheChild (list, pidFile);
                juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
            }

            te::PluginManager& manager;
            std::string formatWord;
            std::string extraFolder;
            juce::File pedal;
            juce::File pidFile;

            /** When the file under scan was handed to the child; nought between files. */
            std::atomic<std::uint32_t> fileStartedMs { 0 };

            /** The files given up on, read after the thread has stopped. */
            std::vector<std::string> skipped;
        };

        /*  THE DEADLINE, on the message thread's timer. A file overdue is
            claimed - its start time taken back to nought, so the thread can
            tell - and the scan under it aborted; Tracktion's coordinator sees
            the abort on its next 10 ms poll and gives the file up. The claim
            is a compare-and-swap on the value read, so a file that finished
            between the read and the claim is never mistaken for a hung one. */
        struct Watchdog final : juce::Timer
        {
            Watchdog (ScanThread& threadToWatch, te::PluginManager& managerToUse)
                : thread (threadToWatch), manager (managerToUse) {}

            void timerCallback() override
            {
                auto started = thread.fileStartedMs.load (std::memory_order_acquire);

                if (started == 0)
                    return;

                const auto elapsed = juce::Time::getMillisecondCounter() - started;

                if (elapsed < static_cast<std::uint32_t> (perFileSeconds) * 1000u)
                    return;

                if (thread.fileStartedMs.compare_exchange_strong (started, 0, std::memory_order_acq_rel)
                    && manager.abortCurrentPluginScan)
                    manager.abortCurrentPluginScan();
            }

            ScanThread& thread;
            te::PluginManager& manager;
        };
    }

    //==============================================================================
    std::vector<std::string> formatWords()
    {
        return { "vst3", "au", "lv2" };
    }

    std::string formatWordOf (const std::string& juceFormatName)
    {
        if (juce::String (juceFormatName).startsWithIgnoreCase ("AudioUnit"))
            return "AU";

        return juceFormatName;
    }

    std::string knownListPath (const std::string& storageFolder)
    {
        return juce::File (juce::String (storageFolder)).getChildFile ("plugins").getChildFile ("known.xml")
                 .getFullPathName().toStdString();
    }

    bool runScanChildIfAsked (int argc, char** argv)
    {
        /*  Tracktion's child looks for its pipe option in the whole command
            line, so the arguments are joined back into one - the reverse of
            what JUCE's own JUCEApplication hands it. */
        juce::String commandLine;

        for (int i = 1; i < argc; ++i)
        {
            if (i > 1)
                commandLine += " ";

            commandLine += juce::String::fromUTF8 (argv[i]);
        }

        if (! commandLine.contains (scanChildOption))
            return false;

        /*  A message loop for the child's sake: it answers the coordinator
            from the message thread. It ends when the coordinator kills it -
            which the coordinator does when its scan is over, and the child
            exits itself on a lost pipe - and, should neither happen, when a
            bounded time has passed: a cap is what keeps a wedged scan from
            leaving a process behind. A plain dispatch loop with a timer that
            stops it, because the modal `runDispatchLoopUntil` is compiled out
            of this build (JUCE_MODAL_LOOPS_PERMITTED=0, a show engine's rule),
            and the child is no exception to it. */
        juce::ScopedJuceInitialiser_GUI juceForTheChild;

        if (! te::PluginManager::startChildProcessPluginScan (commandLine))
            return false;

        /*  Where the parent can find this process should it stop answering;
            see pidFileVariable. */
        if (const auto pidPath = juce::SystemStats::getEnvironmentVariable (pidFileVariable, {});
            pidPath.isNotEmpty())
            juce::File (pidPath).replaceWithText (juce::String (currentProcessId()), false, false, "\n");

       #if JUCE_MAC
        juce::initialiseNSApplication();
       #endif

        StopTheLoop deadline;
        deadline.startTimer (static_cast<int> (std::chrono::milliseconds (std::chrono::minutes (10)).count()));
        juce::MessageManager::getInstance()->runDispatchLoop();

        return true;
    }

    //==============================================================================
    std::vector<KnownPlugin> scanPlugins (const std::string& storageFolder,
                                          const std::string& formatWord,
                                          const std::string& extraFolder,
                                          bool retrySkipped,
                                          std::vector<std::string>& skipped,
                                          std::string& problem)
    {
        problem.clear();
        skipped.clear();

        const auto words = formatWords();

        if (! formatWord.empty() && std::find (words.begin(), words.end(), formatWord) == words.end())
        {
            problem = "unknown format '" + formatWord + "'; one of vst3, au, lv2, or none for every format";
            return {};
        }

        /*  THE SCAN RUNS ON A THREAD OF ITS OWN AND THE MESSAGE LOOP ON THIS
            ONE, which is the shape Tracktion's scanner is written for: the
            coordinator's replies arrive on the pipe's thread and are polled
            with a sleep, so the scan needs no loop, but the known list's
            change message - the one that PERSISTS the list - is delivered
            through the message thread and nowhere else. With the modal loop
            compiled out, the honest way to pump one is to run it, and to hand
            the work to a thread that stops it when it is done. The stop is
            posted behind the last change message, so the list is written
            before the loop returns, and the engine's storage flushes it to
            disk when the engine goes away. */
        juce::ScopedJuceInitialiser_GUI juceForTheScan;

        const juce::File storage { juce::String (storageFolder) };
        auto engine = engineOn (storageFolder);
        auto& manager = engine->getPluginManager();
        manager.setUsesSeparateProcessForScanning (true);

        /*  THE SCAN STARTS FROM THE MACHINE'S LIST, known.xml, rather than
            from whatever Tracktion's own settings happen to hold - which a
            running serve may have written over since (see the header). Each
            file already listed and unchanged is skipped as up to date, so a
            second scan only reads what is new. */
        {
            juce::KnownPluginList seed;

            if (readList (storage, seed))
                if (const auto xml = seed.createXml())
                    manager.knownPluginList.recreateFromXml (*xml);
        }

        if (retrySkipped)
            manager.knownPluginList.clearBlacklistedFiles();

        ScanThread thread (manager, formatWord, extraFolder, storage);
        Watchdog watchdog (thread, manager);
        watchdog.startTimer (250);
        thread.startThread();
        juce::MessageManager::getInstance()->runDispatchLoop();
        thread.stopThread (-1);
        watchdog.stopTimer();

        skipped = thread.skipped;

        if (! writeList (storage, manager.knownPluginList))
            problem = "the scan could not write " + knownListPath (storageFolder);

        return knownFrom (manager.knownPluginList);
    }

    std::vector<KnownPlugin> knownPlugins (const std::string& storageFolder)
    {
        juce::KnownPluginList list;
        readList (juce::File (juce::String (storageFolder)), list);
        return knownFrom (list);
    }

    std::string describePlugin (const std::string& storageFolder, const std::string& identifier)
    {
        juce::KnownPluginList list;
        readList (juce::File (juce::String (storageFolder)), list);

        if (const auto description = list.getTypeForIdentifierString (juce::String (identifier)))
            if (const auto xml = description->createXml())
                return xml->toString().toStdString();

        return {};
    }

    std::vector<std::string> skippedPlugins (const std::string& storageFolder)
    {
        juce::KnownPluginList list;
        readList (juce::File (juce::String (storageFolder)), list);
        std::vector<std::string> out;

        for (const auto& file : list.getBlacklistedFiles())
            out.push_back (file.toStdString());

        return out;
    }
}
