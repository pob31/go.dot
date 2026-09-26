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
#include <map>
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

        /*  WHERE EACH LV2 WAS FOUND, by its URI (2026-09-26). An LV2 is
            named by a URI, not a file, and a child can only make one whose
            bundle its LV2 world has loaded - the default folders and
            LV2_PATH, and nothing a scan was pointed at with --path. So the
            scan remembers the bundle folder of every LV2 it found, as a
            `bundle` attribute on the plugin's element in known.xml, and the
            description a child is handed carries it. JUCE's own reading of
            the list passes over an attribute it does not know. */
        using Bundles = std::map<juce::String, juce::String>;

        constexpr const char* bundleAttribute = "bundle";

        Bundles bundlesIn (const juce::XmlElement& list)
        {
            Bundles out;

            for (const auto* element : list.getChildWithTagNameIterator ("PLUGIN"))
                if (element->hasAttribute (bundleAttribute))
                    out[element->getStringAttribute ("file")] = element->getStringAttribute (bundleAttribute);

            return out;
        }

        KnownPlugin knownFrom (const juce::PluginDescription& description, const Bundles& bundles)
        {
            KnownPlugin out;
            out.name = description.name.toStdString();
            out.identifier = description.createIdentifierString().toStdString();
            out.format = formatWordOf (description.pluginFormatName.toStdString());
            out.manufacturer = description.manufacturerName.toStdString();
            out.path = description.fileOrIdentifier.toStdString();

            if (const auto xml = description.createXml())
            {
                if (const auto bundle = bundles.find (description.fileOrIdentifier); bundle != bundles.end())
                    xml->setAttribute (bundleAttribute, bundle->second);

                out.description = xml->toString().toStdString();
            }

            return out;
        }

        std::vector<KnownPlugin> knownFrom (const juce::KnownPluginList& list, const Bundles& bundles)
        {
            std::vector<KnownPlugin> out;

            for (const auto& description : list.getTypes())
                out.push_back (knownFrom (description, bundles));

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
        bool readList (const juce::File& storage, juce::KnownPluginList& list, Bundles& bundles)
        {
            bundles.clear();

            if (const auto own = juce::parseXML (juce::File (juce::String (knownListPath (storage.getFullPathName().toStdString()))));
                own != nullptr)
            {
                list.recreateFromXml (*own);
                bundles = bundlesIn (*own);
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
        bool writeList (const juce::File& storage, const juce::KnownPluginList& list, const Bundles& bundles)
        {
            const juce::File file { juce::String (knownListPath (storage.getFullPathName().toStdString())) };
            file.getParentDirectory().createDirectory();

            if (const auto xml = list.createXml())
            {
                for (auto* element : xml->getChildWithTagNameIterator ("PLUGIN"))
                    if (const auto bundle = bundles.find (element->getStringAttribute ("file")); bundle != bundles.end())
                        element->setAttribute (bundleAttribute, bundle->second);

                return xml->writeTo (file);
            }

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
            stop when done, behind the known list's last change message.

            THE WORK IS LISTED FIRST (2026-09-26), every format's files before
            the first is scanned, so a scan launched by the app can say "12 of
            140" from the start; progress is written after every file, and the
            stop file is looked for between two. A retry scans one file, with
            the first format that claims it. */
        struct ScanThread final : juce::Thread
        {
            ScanThread (te::PluginManager& managerToUse, ScanOptions optionsToUse, juce::File storageToUse)
                : juce::Thread ("wfg plugin scan"),
                  manager (managerToUse),
                  options (std::move (optionsToUse)),
                  pedal (storageToUse.getChildFile ("plugin-scan.pedal")),
                  pidFile (storageToUse.getChildFile ("plugin-scan.child"))
            {
                setEnvironment (pidFileVariable, pidFile.getFullPathName());
            }

            /** Where the scan is, for whoever asked; a scan run by hand has no file. */
            void publish()
            {
                progress.skipped = skipped;
                progress.found = manager.knownPluginList.getNumTypes();

                if (! options.progressFile.empty())
                    juce::File (juce::String (options.progressFile))
                        .replaceWithText (juce::String (progress.toJson()), false, false, "\n");
            }

            bool stopAsked() const
            {
                return ! options.stopFile.empty() && juce::File (juce::String (options.stopFile)).existsAsFile();
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

                std::vector<std::pair<juce::AudioPluginFormat*, juce::String>> work;
                const juce::String retry { options.retryFile };

                for (int i = 0; i < manager.pluginFormatManager.getNumFormats() && ! threadShouldExit(); ++i)
                {
                    auto* format = manager.pluginFormatManager.getFormat (i);

                    if (format == nullptr || ! formatMatches (*format, options.formatWord))
                        continue;

                    /*  Tracktion's built-in format has nothing to scan, and
                        asking it for default locations answers nothing;
                        skipped by name. */
                    if (format->getName() == te::PluginManager::builtInPluginFormatName)
                        continue;

                    if (retry.isNotEmpty())
                    {
                        if (work.empty() && format->fileMightContainThisPluginType (retry))
                            work.emplace_back (format, retry);

                        continue;
                    }

                    auto where = format->getDefaultLocationsToSearch();

                    if (! options.extraFolder.empty())
                        where.addIfNotAlreadyThere (juce::File (juce::String (options.extraFolder)));

                    /*  AN AU HAS NO FOLDER TO SEARCH: the system registers
                        components and JUCE's AU format lists them, whatever
                        path it is handed. Every other format with nowhere to
                        look has nothing to find. */
                    if (where.getNumPaths() == 0 && ! format->getName().startsWith ("AudioUnit"))
                        continue;

                    /*  ONCE EACH: an LV2 bundle holding two plugins is answered
                        twice, once per plugin, by JUCE's search. */
                    auto files = format->searchPathsForPlugins (where, true, false);

                    /*  AND EVERY LV2 BUNDLE BY ITS FOLDER, beside JUCE's search
                        (2026-09-26): on macOS that search answered nothing for
                        a folder handed to it, where Windows and Linux listed the
                        bundle - while asking for a bundle by its folder, which
                        is what each file below is, worked everywhere. So the
                        folders are listed here too, and the de-duplication
                        below makes the two lists one. */
                    if (format->getName() == "LV2")
                        for (int p = 0; p < where.getNumPaths(); ++p)
                            for (const auto& bundle : where[p].findChildFiles (juce::File::findDirectories, false, "*.lv2"))
                                files.add (bundle.getFullPathName());

                    files.removeDuplicates (false);

                    for (const auto& file : files)
                        work.emplace_back (format, file);
                }

                progress.state = "scanning";
                progress.total = static_cast<int> (work.size());
                publish();

                for (const auto& [format, file] : work)
                {
                    if (threadShouldExit())
                        break;

                    if (stopAsked())
                    {
                        stopped = true;
                        break;
                    }

                    progress.file = file.toStdString();
                    progress.format = formatWordOf (format->getName().toStdString());

                    if (retry.isEmpty() && (list.getBlacklistedFiles().contains (file) || list.isListingUpToDate (file, *format)))
                    {
                        ++progress.done;
                        continue;
                    }

                    publish();
                    pedal.replaceWithText (file, false, false, "\n");
                    fileStartedMs.store (std::max<std::uint32_t> (1u, juce::Time::getMillisecondCounter()),
                                         std::memory_order_release);

                    juce::OwnedArray<juce::PluginDescription> found;
                    list.scanAndAddFile (file, true, found, *format);

                    if (format->getName() == "LV2")
                        for (const auto* description : found)
                            bundles[description->fileOrIdentifier] = file;

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

                    ++progress.done;
                    publish();
                }

                progress.file.clear();
                putDownTheChild (list, pidFile);
                juce::MessageManager::callAsync ([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
            }

            te::PluginManager& manager;
            ScanOptions options;
            juce::File pedal;
            juce::File pidFile;

            ScanProgress progress;
            bool stopped = false;

            /** When the file under scan was handed to the child; nought between files. */
            std::atomic<std::uint32_t> fileStartedMs { 0 };

            /** The files given up on, read after the thread has stopped. */
            std::vector<std::string> skipped;

            /** Every LV2's bundle, seeded from the list and added to as found. */
            Bundles bundles;
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

    std::string setAsideLv2PathOnWindows()
    {
       #if JUCE_WINDOWS
        const auto held = juce::SystemStats::getEnvironmentVariable ("LV2_PATH", {});

        if (held.isEmpty())
            return {};

        ::_putenv_s ("LV2_PATH", "");
        return held.toStdString();
       #else
        return {};
       #endif
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
    std::string ScanProgress::toJson() const
    {
        auto object = std::make_unique<juce::DynamicObject>();
        object->setProperty ("state", juce::String (state));
        object->setProperty ("format", juce::String (format));
        object->setProperty ("file", juce::String (file));
        object->setProperty ("done", done);
        object->setProperty ("total", total);
        object->setProperty ("found", found);

        juce::Array<juce::var> files;

        for (const auto& one : skipped)
            files.add (juce::String (one));

        object->setProperty ("skipped", files);
        return juce::JSON::toString (juce::var (object.release()), true).toStdString();
    }

    bool ScanProgress::fromJson (const std::string& text, ScanProgress& out)
    {
        const auto parsed = juce::JSON::parse (juce::String (text));
        const auto* object = parsed.getDynamicObject();

        if (object == nullptr || ! object->hasProperty ("state"))
            return false;

        out = {};
        out.state = object->getProperty ("state").toString().toStdString();
        out.format = object->getProperty ("format").toString().toStdString();
        out.file = object->getProperty ("file").toString().toStdString();
        out.done = static_cast<int> (object->getProperty ("done"));
        out.total = static_cast<int> (object->getProperty ("total"));
        out.found = static_cast<int> (object->getProperty ("found"));

        if (const auto* files = object->getProperty ("skipped").getArray())
            for (const auto& one : *files)
                out.skipped.push_back (one.toString().toStdString());

        return true;
    }

    bool readScanProgress (const std::string& path, ScanProgress& out)
    {
        const juce::File file { juce::String (path) };
        return file.existsAsFile() && ScanProgress::fromJson (file.loadFileAsString().toStdString(), out);
    }

    std::vector<KnownPlugin> scanPlugins (const std::string& storageFolder,
                                          const std::string& formatWord,
                                          const std::string& extraFolder,
                                          bool retrySkipped,
                                          std::vector<std::string>& skipped,
                                          std::string& problem)
    {
        ScanOptions options;
        options.formatWord = formatWord;
        options.extraFolder = extraFolder;
        options.retrySkipped = retrySkipped;
        return scanPlugins (storageFolder, options, skipped, problem);
    }

    std::vector<KnownPlugin> scanPlugins (const std::string& storageFolder, const ScanOptions& options,
                                          std::vector<std::string>& skipped, std::string& problem)
    {
        problem.clear();
        skipped.clear();

        const auto words = formatWords();
        const auto& formatWord = options.formatWord;

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
        Bundles bundles;

        {
            juce::KnownPluginList seed;

            if (readList (storage, seed, bundles))
                if (const auto xml = seed.createXml())
                    manager.knownPluginList.recreateFromXml (*xml);
        }

        if (options.retrySkipped)
            manager.knownPluginList.clearBlacklistedFiles();

        if (! options.retryFile.empty())
            manager.knownPluginList.removeFromBlacklist (juce::String (options.retryFile));

        ScanThread thread (manager, options, storage);
        thread.bundles = bundles;
        Watchdog watchdog (thread, manager);
        watchdog.startTimer (250);
        thread.startThread();
        juce::MessageManager::getInstance()->runDispatchLoop();
        thread.stopThread (-1);
        watchdog.stopTimer();

        skipped = thread.skipped;

        if (! writeList (storage, manager.knownPluginList, thread.bundles))
            problem = "the scan could not write " + knownListPath (storageFolder);

        /*  THE LAST WORD, after the list is on disk: whoever reads `finished`
            and then the list finds this scan's list. */
        thread.progress.state = thread.stopped ? "stopped" : "finished";
        thread.publish();

        return knownFrom (manager.knownPluginList, thread.bundles);
    }

    std::vector<KnownPlugin> knownPlugins (const std::string& storageFolder)
    {
        juce::KnownPluginList list;
        Bundles bundles;
        readList (juce::File (juce::String (storageFolder)), list, bundles);
        return knownFrom (list, bundles);
    }

    std::string describePlugin (const std::string& storageFolder, const std::string& identifier)
    {
        juce::KnownPluginList list;
        Bundles bundles;
        readList (juce::File (juce::String (storageFolder)), list, bundles);

        if (const auto description = list.getTypeForIdentifierString (juce::String (identifier)))
            return knownFrom (*description, bundles).description;

        return {};
    }

    std::vector<std::string> skippedPlugins (const std::string& storageFolder)
    {
        juce::KnownPluginList list;
        Bundles bundles;
        readList (juce::File (juce::String (storageFolder)), list, bundles);
        std::vector<std::string> out;

        for (const auto& file : list.getBlacklistedFiles())
            out.push_back (file.toStdString());

        return out;
    }
}
